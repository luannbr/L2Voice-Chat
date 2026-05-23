// Package router implements the pure routing decision function for
// inbound voice packets. Given a packet, the sender's identity and an
// immutable snapshot of the world, Route returns the list of
// recipients with the egress channel and post-prefs volume that
// should be applied at the client.
//
// The function is pure (no side effects, no I/O). It is meant to be
// invoked from the UDP audio handler on every speaker burst, so its
// hot path uses only world.WorldState read methods (RWMutex RLock
// inside accessors).
//
// All rule numbering below refers to Voice_System/Prompt.txt.
package router

import (
	"math"

	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// Packet describes the inbound packet to be routed. Fields that aren't
// in the wire payload (e.g. sender position) are filled in by the UDP
// handler from the latest world snapshot of the sender BEFORE calling
// Route, so routing decisions never depend on stale per-packet data.
type Packet struct {
	SenderID   uint32
	Channel    world.Channel // ingress channel (what the speaker pressed PTT for)
	InstanceID uint32        // sender's instance — pinned at packet time
	X, Y, Z    float32       // sender's position — pinned at packet time

	// WhisperActive is true when the audio layer determined that the
	// sender is leader/sub-leader AND their whisper config is currently
	// transmitting (always-open, or PTT key held). The router uses
	// this to force CLAN-with-override regardless of which channel
	// the sender's PTT key matches.
	WhisperActive bool
}

// Decision is the per-recipient routing result.
type Decision struct {
	RecipientID uint32

	// EgressChan is the channel the recipient's client should display
	// the packet on (it may differ from the ingress channel — e.g.
	// whisper rewrites to CLAN, mode rewrites to ChModeUnified).
	EgressChan world.Channel

	// Volume is the multiplicative factor (typically 0..2) derived
	// from the recipient's channel volume * per-player volume on
	// sender. Distance attenuation is folded in for PROXIMITY.
	Volume float32

	// ForceAudible signals that an override (leader speaking on
	// CLAN/ALLY, or any mode-unified packet) requires the client to
	// play the packet even if its ChannelEnabled toggle for EgressChan
	// is off. Per-player mute / volume still apply.
	ForceAudible bool
}

// Config carries router tunables.
type Config struct {
	// ProximityMaxDist is the maximum distance (game units, 1 unit ≈ 1cm)
	// at which a proximity packet is delivered. Linear attenuation is
	// applied: Volume *= (1 - dist/MaxDist).
	ProximityMaxDist float32
}

// DefaultConfig returns sensible defaults.
func DefaultConfig() Config {
	return Config{ProximityMaxDist: 1500.0}
}

// Route is the pure routing function. Returns nil if there are no
// recipients (sender unknown, channel inactive, or remote-muted).
func Route(pkt Packet, w *world.WorldState, cfg Config) []Decision {
	sender := w.Player(pkt.SenderID)
	if sender == nil {
		return nil
	}
	role := w.RoleOf(pkt.SenderID)
	isPriv := role == world.RoleLeader || role == world.RoleSubLeader

	// ---- Whisper override (Regra 4) ------------------------------
	// Leader/sub with whisper active routes as CLAN with override,
	// regardless of which ingress channel was pressed. Whisper is a
	// transmission mode, not a separate channel.
	if pkt.WhisperActive && isPriv && sender.ClanID != 0 {
		if w.IsRemotelyMuted(pkt.SenderID, "clan") {
			return nil
		}
		return routeClanOrAlly(sender, w, true, world.ChClan, sender.ClanID, w.ClanMembers)
	}

	// ---- Mode unified (Regra 6) ----------------------------------
	// If sender's clan has an active mode (and clan isn't opted out),
	// CLAN/ALLY ingress collapses into the unified mode channel that
	// always overrides toggles. PROXIMITY and PARTY are unaffected.
	if pkt.Channel == world.ChClan || pkt.Channel == world.ChAlly {
		if modeActive(sender, w) {
			// Remote mute on either clan or ally scope silences the
			// speaker in unified mode (which spans both scopes).
			if w.IsRemotelyMuted(pkt.SenderID, "clan") ||
				w.IsRemotelyMuted(pkt.SenderID, "ally") {
				return nil
			}
			return routeMode(sender, w)
		}
	}

	switch pkt.Channel {
	case world.ChProximity:
		return routeProximity(pkt, sender, w, cfg)
	case world.ChParty:
		return routeParty(sender, w)
	case world.ChClan:
		if w.IsRemotelyMuted(pkt.SenderID, "clan") {
			return nil
		}
		return routeClanOrAlly(sender, w, isPriv, world.ChClan, sender.ClanID, w.ClanMembers)
	case world.ChAlly:
		if w.IsRemotelyMuted(pkt.SenderID, "ally") {
			return nil
		}
		return routeClanOrAlly(sender, w, isPriv, world.ChAlly, sender.AllyID, w.AllyMembers)
	case world.ChCC:
		// Sender must be CC leader or have explicit speak permission.
		// Unauthorized speakers drop entirely (no audible side-effect
		// from forgery — server is source of truth).
		if !w.CCSpeakAllowed(pkt.SenderID) {
			return nil
		}
		return routeCC(sender, w)
	}
	return nil
}

// modeActive reports whether the sender should have CLAN/ALLY traffic
// rewritten to the unified mode channel.
func modeActive(sender *world.Player, w *world.WorldState) bool {
	if sender.ClanID == 0 {
		return false
	}
	c := w.Clan(sender.ClanID)
	if c == nil {
		return false
	}
	return c.Mode != world.ModeNone && !c.OptedOutOfUnifiedMode
}

// routeProximity walks all players in the sender's instance, drops
// out-of-range, applies recipient toggle + per-player mute, computes
// volume with linear distance attenuation.
//
// Regra 8: proximity is NOT affected by remote mute and never carries
// mode audio (which is short-circuited above before reaching here).
func routeProximity(pkt Packet, sender *world.Player, w *world.WorldState, cfg Config) []Decision {
	candidates := w.PlayersInInstance(pkt.InstanceID)
	if len(candidates) == 0 {
		return nil
	}
	out := make([]Decision, 0, len(candidates))
	maxD := cfg.ProximityMaxDist
	if maxD <= 0 {
		maxD = DefaultConfig().ProximityMaxDist
	}
	for _, p := range candidates {
		if p.ID == sender.ID {
			continue
		}
		dx, dy, dz := p.X-pkt.X, p.Y-pkt.Y, p.Z-pkt.Z
		dist := float32(math.Sqrt(float64(dx*dx + dy*dy + dz*dz)))
		if dist > maxD {
			continue
		}
		if !p.Prefs.ChannelEnabled[world.ChProximity] {
			continue
		}
		if p.Prefs.PerPlayerMute[sender.ID] {
			continue
		}
		atten := 1.0 - dist/maxD
		if atten < 0 {
			atten = 0
		}
		vol := p.Prefs.ChannelVolume[world.ChProximity] *
			perPlayerVol(p, sender.ID) *
			atten
		out = append(out, Decision{
			RecipientID: p.ID,
			EgressChan:  world.ChProximity,
			Volume:      vol,
		})
	}
	return out
}

// routeParty — Regra 7. No override, no remote-mute scoping (remote
// mute applies only to CLAN/ALLY per Regra 5).
func routeParty(sender *world.Player, w *world.WorldState) []Decision {
	members := w.PartyMembers(sender.ID)
	if len(members) == 0 {
		return nil
	}
	out := make([]Decision, 0, len(members))
	for _, id := range members {
		if id == sender.ID {
			continue
		}
		p := w.Player(id)
		if p == nil || p.Prefs == nil {
			continue
		}
		if !p.Prefs.ChannelEnabled[world.ChParty] {
			continue
		}
		if p.Prefs.PerPlayerMute[sender.ID] {
			continue
		}
		vol := p.Prefs.ChannelVolume[world.ChParty] * perPlayerVol(p, sender.ID)
		out = append(out, Decision{
			RecipientID: id,
			EgressChan:  world.ChParty,
			Volume:      vol,
		})
	}
	return out
}

// routeClanOrAlly is shared between CLAN and ALLY channels — they
// follow the same rules (Regra 3): scope-specific membership, leader
// override of Disabled, per-player mute/volume always honored.
//
// memberFn is the world accessor for the scope's member list.
func routeClanOrAlly(sender *world.Player, w *world.WorldState,
	override bool, ch world.Channel,
	scopeID uint32, memberFn func(uint32) []uint32) []Decision {
	if scopeID == 0 {
		return nil
	}
	members := memberFn(scopeID)
	if len(members) == 0 {
		return nil
	}
	out := make([]Decision, 0, len(members))
	for _, id := range members {
		if id == sender.ID {
			continue
		}
		p := w.Player(id)
		if p == nil || p.Prefs == nil {
			continue
		}
		enabled := p.Prefs.ChannelEnabled[ch]
		if !enabled && !override {
			continue
		}
		if p.Prefs.PerPlayerMute[sender.ID] {
			continue
		}
		vol := p.Prefs.ChannelVolume[ch] * perPlayerVol(p, sender.ID)
		out = append(out, Decision{
			RecipientID:  id,
			EgressChan:   ch,
			Volume:       vol,
			ForceAudible: override && !enabled,
		})
	}
	return out
}

// routeMode unifies clan + ally membership and always sets
// ForceAudible (Regra 6: "ignorando toggles").
func routeMode(sender *world.Player, w *world.WorldState) []Decision {
	seen := map[uint32]struct{}{}
	for _, id := range w.ClanMembers(sender.ClanID) {
		seen[id] = struct{}{}
	}
	if sender.AllyID != 0 {
		for _, id := range w.AllyMembers(sender.AllyID) {
			seen[id] = struct{}{}
		}
	}
	delete(seen, sender.ID)
	if len(seen) == 0 {
		return nil
	}
	out := make([]Decision, 0, len(seen))
	for id := range seen {
		p := w.Player(id)
		if p == nil || p.Prefs == nil {
			continue
		}
		if p.Prefs.PerPlayerMute[sender.ID] {
			continue
		}
		// Volume basis: recipient's CLAN channel volume (mode replaces
		// CLAN/ALLY; CLAN is the more universal of the two since
		// every member is in a clan but not every clan is in an ally).
		vol := p.Prefs.ChannelVolume[world.ChClan] * perPlayerVol(p, sender.ID)
		out = append(out, Decision{
			RecipientID:  id,
			EgressChan:   world.ChModeUnified,
			Volume:       vol,
			ForceAudible: true,
		})
	}
	return out
}

// routeCC delivers to every other member of the sender's command
// channel. Members may turn down the channel volume but cannot
// "disable" CC (it's leader-broadcast-style); ForceAudible is set so
// the client renders even when the toggle says off — mirrors the
// override semantics of leader-on-CLAN (Prompt §Regra 3 analog).
func routeCC(sender *world.Player, w *world.WorldState) []Decision {
	cc := w.CommandChannel(sender.ID)
	if cc == nil {
		return nil
	}
	out := make([]Decision, 0, len(cc.MemberIDs))
	for _, id := range cc.MemberIDs {
		if id == sender.ID {
			continue
		}
		p := w.Player(id)
		if p == nil || p.Prefs == nil {
			continue
		}
		if p.Prefs.PerPlayerMute[sender.ID] {
			continue
		}
		// Recipient channel volume: use CLAN's slot (clients render CC
		// alongside clan/ally semantically; we don't have a dedicated
		// CC volume slot in the 4-channel pref array).
		vol := p.Prefs.ChannelVolume[world.ChClan] * perPlayerVol(p, sender.ID)
		out = append(out, Decision{
			RecipientID:  id,
			EgressChan:   world.ChCC,
			Volume:       vol,
			ForceAudible: true,
		})
	}
	return out
}

func perPlayerVol(p *world.Player, senderID uint32) float32 {
	if v, ok := p.Prefs.PerPlayerVolume[senderID]; ok {
		return v
	}
	return 1.0
}
