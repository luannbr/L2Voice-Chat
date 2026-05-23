// Client-state snapshot pushes (Phase D server side).
//
// On auth and on every world event that changes a player's clan/ally/
// party/CC membership, mode, or sub-leader role, the server builds a
// per-client snapshot and sends it. Clients use it to drive their
// tab membership lists, leader panel visibility, mode banner, and CC
// tab visibility.
//
// "voice_active" is included per member so the DLL knows which entries
// have a live voice session (= worth showing in the per-tab roster).
// Members who are in the L2 group but offline-for-voice still appear
// dimmed so the user can see they're around.
package control

import (
	"log"

	"github.com/luannbr/l2voice/voice-service/internal/topology"
	"github.com/luannbr/l2voice/voice-service/internal/world"
)

type stateMember struct {
	ID          uint32 `json:"id"`
	VoiceActive bool   `json:"voice_active"`
	Role        uint8  `json:"role,omitempty"`        // clan only: 0=member, 1=sub, 2=leader
	CanSpeak    bool   `json:"can_speak,omitempty"`   // CC only
	RemoteMuted bool   `json:"remote_muted,omitempty"`// clan/ally only — set by a leader/sub via clan_remote_mute
}

type clientStateFrame struct {
	Type           string        `json:"type"`
	Role           uint8         `json:"role"`
	ClanID         uint32        `json:"clan_id"`
	AllyID         uint32        `json:"ally_id"`
	PartyID        uint64        `json:"party_id"`
	CCID           uint32        `json:"cc_id"`
	CCLeaderID     uint32        `json:"cc_leader_id"`
	CCCanSpeak     bool          `json:"cc_can_speak"`
	ClanMode       uint8         `json:"clan_mode"`
	OptedOut       bool          `json:"opted_out"`
	PartyMembers   []stateMember `json:"party_members"`
	ClanMembers    []stateMember `json:"clan_members"`
	AllyMembers    []stateMember `json:"ally_members"`
	CCMembers      []stateMember `json:"cc_members"`
}

// buildClientState assembles the snapshot for `playerID`. Reads world
// state + topology; no side effects.
func buildClientState(playerID uint32, w *world.WorldState, topo *topology.State) clientStateFrame {
	p := w.Player(playerID)
	if p == nil {
		return clientStateFrame{Type: "client_state"}
	}
	out := clientStateFrame{
		Type:    "client_state",
		Role:    uint8(w.RoleOf(playerID)),
		ClanID:  p.ClanID,
		AllyID:  p.AllyID,
		PartyID: p.PartyID,
	}
	if c := w.Clan(p.ClanID); c != nil {
		out.ClanMode = uint8(c.Mode)
		out.OptedOut = c.OptedOutOfUnifiedMode
	}
	if cc := w.CommandChannel(playerID); cc != nil {
		_, hasGrant := cc.SpeakPermissions[playerID]
		out.CCID = cc.ID
		out.CCLeaderID = cc.LeaderID
		out.CCCanSpeak = playerID == cc.LeaderID || hasGrant
		out.CCMembers = collectCCMembers(cc, topo)
	}
	if p.PartyID != 0 {
		out.PartyMembers = collectMembersByIDs(w.PartyMembers(playerID), topo, w, false, "")
	}
	if p.ClanID != 0 {
		out.ClanMembers = collectMembersByIDs(w.ClanMembers(p.ClanID), topo, w, true, "clan")
	}
	if p.AllyID != 0 {
		out.AllyMembers = collectMembersByIDs(w.AllyMembers(p.AllyID), topo, w, false, "ally")
	}
	return out
}

func collectMembersByIDs(ids []uint32, topo *topology.State,
	w *world.WorldState, withRole bool, scope string) []stateMember {
	out := make([]stateMember, 0, len(ids))
	for _, id := range ids {
		m := stateMember{
			ID:          id,
			VoiceActive: topo.LookupByPlayer(id) != nil,
		}
		if withRole {
			m.Role = uint8(w.RoleOf(id))
		}
		if scope != "" {
			m.RemoteMuted = w.IsRemotelyMuted(id, scope)
		}
		out = append(out, m)
	}
	return out
}

func collectCCMembers(cc *world.CommandChannel, topo *topology.State) []stateMember {
	out := make([]stateMember, 0, len(cc.MemberIDs))
	for _, id := range cc.MemberIDs {
		_, hasGrant := cc.SpeakPermissions[id]
		out = append(out, stateMember{
			ID:          id,
			VoiceActive: topo.LookupByPlayer(id) != nil,
			CanSpeak:    id == cc.LeaderID || hasGrant,
		})
	}
	return out
}

// pushClientStateTo sends a fresh snapshot to a single player. Safe
// to call when the player has no client registered (silently no-ops).
func pushClientStateTo(dep deps, playerID uint32) {
	c := dep.reg.ClientByPlayer(playerID)
	if c == nil {
		return
	}
	_ = c.Send(buildClientState(playerID, dep.world, dep.topo))
}

// pushClientStateToGroup pushes to every player id in the list whose
// client is registered. Used after clan-wide mutations (mode, sub-
// leader changes, etc.) so every member's GUI updates simultaneously.
func pushClientStateToGroup(dep deps, ids []uint32) {
	for _, id := range ids {
		pushClientStateTo(dep, id)
	}
}

// PushPlayerState is the public entry point used by other packages
// (notably the Redis subscriber in `social`) to nudge a client when
// a world event has changed its view. Signature is intentionally
// flat so social.go doesn't have to know about deps.
func PushPlayerState(reg *ConnReg, w *world.WorldState,
	topo *topology.State, playerID uint32) {
	if reg == nil {
		return
	}
	c := reg.ClientByPlayer(playerID)
	if c == nil {
		// Player is not (yet) authenticated for voice. The state will
		// be picked up on their next auth via the initial snapshot.
		log.Printf("state_push: SKIP player=%d (no voice session)", playerID)
		return
	}
	frame := buildClientState(playerID, w, topo)
	log.Printf("state_push: player=%d clan=%d ally=%d party=%d cc=%d members(party/clan/ally/cc)=%d/%d/%d/%d",
		playerID, frame.ClanID, frame.AllyID, frame.PartyID, frame.CCID,
		len(frame.PartyMembers), len(frame.ClanMembers),
		len(frame.AllyMembers), len(frame.CCMembers))
	_ = c.Send(frame)
}

// PushPlayerStateToGroup pushes to multiple players at once. Same
// semantics as the unexported pushClientStateToGroup but accessible
// from outside the package.
func PushPlayerStateToGroup(reg *ConnReg, w *world.WorldState,
	topo *topology.State, ids []uint32) {
	for _, id := range ids {
		PushPlayerState(reg, w, topo, id)
	}
}
