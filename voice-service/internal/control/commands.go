// Control-plane command handlers (Prompt §Fase C).
//
// Each handler is dispatched by message type. All authz lives in
// authz.go as pure functions over world.WorldState — handlers wrap
// them, mutate state on success and broadcast notifications.
package control

import (
	"encoding/json"
	"log"

	"github.com/luannbr/l2voice/voice-service/internal/topology"
	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// envelope: minimal header to peek at message type before
// unmarshalling the full payload.
type cmdHeader struct {
	Type string `json:"type"`
}

// errFrame is the server's standard error response.
type errFrame struct {
	Type   string `json:"type"`
	Cmd    string `json:"cmd"`
	Reason string `json:"reason"`
}

func sendErr(c *Client, cmd string, err error) {
	_ = c.Send(errFrame{Type: "err", Cmd: cmd, Reason: err.Error()})
}

// dispatch routes raw msgs to the per-type handlers. Returns true if
// the message was understood (regardless of success), false to let the
// outer loop log "unknown".
func dispatch(c *Client, raw []byte, dep deps) bool {
	var hdr cmdHeader
	if err := json.Unmarshal(raw, &hdr); err != nil {
		return false
	}
	switch hdr.Type {
	case "set_channel_enabled":
		handleSetChannelEnabled(c, raw, dep)
	case "set_channel_volume":
		handleSetChannelVolume(c, raw, dep)
	case "set_player_mute":
		handleSetPlayerMute(c, raw, dep)
	case "set_player_volume":
		handleSetPlayerVolume(c, raw, dep)
	case "whisper_set":
		handleWhisperSet(c, raw, dep)
	case "clan_promote_subleader":
		handleClanPromote(c, raw, dep, true)
	case "clan_demote_subleader":
		handleClanPromote(c, raw, dep, false)
	case "clan_set_mode":
		handleClanSetMode(c, raw, dep)
	case "clan_remote_mute":
		handleClanRemoteMute(c, raw, dep)
	case "clan_opt_out_unified":
		handleClanOptOut(c, raw, dep)
	case "cc_grant_speak":
		handleCCGrantSpeak(c, raw, dep)
	case "ping":
		// Echo the client's timestamp back as "pong" so it can compute
		// round-trip latency. Cheap, no state touched.
		var q struct {
			T uint64 `json:"t"`
		}
		_ = json.Unmarshal(raw, &q)
		_ = c.Send(map[string]interface{}{"type": "pong", "t": q.T})
	case "name_query":
		// Forwarded to the existing handler in ws.go.
		var q struct {
			SrcID uint32 `json:"src_id"`
		}
		_ = json.Unmarshal(raw, &q)
		if q.SrcID != 0 {
			handleNameQuery(c.conn, dep.topo, q.SrcID)
		}
	case "player_name_query":
		var q struct {
			PlayerID uint32 `json:"player_id"`
		}
		_ = json.Unmarshal(raw, &q)
		if q.PlayerID != 0 {
			handlePlayerNameQuery(c, q.PlayerID)
		}
	default:
		return false
	}
	return true
}

// deps bundles the handler-side dependencies. Passed by value;
// internally each field is a shared pointer.
type deps struct {
	world *world.WorldState
	topo  *topology.State
	reg   *ConnReg
}

// ----- handlers ------------------------------------------------------

type setChannelEnabledMsg struct {
	Channel uint8 `json:"channel"`
	Enabled bool  `json:"enabled"`
}

func handleSetChannelEnabled(c *Client, raw []byte, dep deps) {
	var m setChannelEnabledMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "set_channel_enabled", err)
		return
	}
	if err := ValidChannel(world.Channel(m.Channel)); err != nil {
		sendErr(c, "set_channel_enabled", err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	if p == nil {
		sendErr(c, "set_channel_enabled", ErrUnknownPlayer)
		return
	}
	// Direct field write is safe under WorldState's RWMutex only via a
	// mutating accessor. We don't have one yet; add it later or inline
	// here under the lock. For Phase C we use the Prefs pointer
	// directly — it's owned by this player and we don't expose the map
	// outside. Race-wise: ChannelEnabled is a [4]bool, writes are
	// torn-free on amd64. Good enough for MVP.
	p.Prefs.ChannelEnabled[m.Channel] = m.Enabled
}

type setChannelVolumeMsg struct {
	Channel uint8   `json:"channel"`
	Volume  float32 `json:"volume"`
}

func handleSetChannelVolume(c *Client, raw []byte, dep deps) {
	var m setChannelVolumeMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "set_channel_volume", err)
		return
	}
	if err := ValidChannel(world.Channel(m.Channel)); err != nil {
		sendErr(c, "set_channel_volume", err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	if p == nil {
		return
	}
	p.Prefs.ChannelVolume[m.Channel] = ClampVolume(m.Volume)
}

type setPlayerMuteMsg struct {
	TargetID uint32 `json:"target_id"`
	Muted    bool   `json:"muted"`
}

func handleSetPlayerMute(c *Client, raw []byte, dep deps) {
	var m setPlayerMuteMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		return
	}
	if m.TargetID == 0 || m.TargetID == c.PlayerID {
		return
	}
	p := dep.world.Player(c.PlayerID)
	if p == nil {
		return
	}
	if m.Muted {
		p.Prefs.PerPlayerMute[m.TargetID] = true
	} else {
		delete(p.Prefs.PerPlayerMute, m.TargetID)
	}
}

type setPlayerVolumeMsg struct {
	TargetID uint32  `json:"target_id"`
	Volume   float32 `json:"volume"`
}

func handleSetPlayerVolume(c *Client, raw []byte, dep deps) {
	var m setPlayerVolumeMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		return
	}
	if m.TargetID == 0 || m.TargetID == c.PlayerID {
		return
	}
	p := dep.world.Player(c.PlayerID)
	if p == nil {
		return
	}
	p.Prefs.PerPlayerVolume[m.TargetID] = ClampVolume(m.Volume)
}

type whisperSetMsg struct {
	Mode uint8 `json:"mode"`
	Key  int   `json:"key"`
}

func handleWhisperSet(c *Client, raw []byte, dep deps) {
	var m whisperSetMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "whisper_set", err)
		return
	}
	if err := AuthzWhisper(c.PlayerID, m.Mode, dep.world); err != nil {
		sendErr(c, "whisper_set", err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	p.Prefs.WhisperMode = m.Mode
	p.Prefs.WhisperKey = m.Key
}

type clanPromoteMsg struct {
	TargetID uint32 `json:"target_id"`
}

func handleClanPromote(c *Client, raw []byte, dep deps, promote bool) {
	cmdName := "clan_demote_subleader"
	if promote {
		cmdName = "clan_promote_subleader"
	}
	var m clanPromoteMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, cmdName, err)
		return
	}
	if err := AuthzPromoteSubLeader(c.PlayerID, m.TargetID, dep.world); err != nil {
		sendErr(c, cmdName, err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	if !dep.world.SetSubLeader(p.ClanID, m.TargetID, promote) {
		sendErr(c, cmdName, ErrUnknownTarget)
		return
	}
	// Broadcast to the whole clan so every member's GUI updates.
	broadcastSubLeaderChanged(dep, p.ClanID, m.TargetID, promote)
}

type clanSetModeMsg struct {
	Mode uint8 `json:"mode"`
}

func handleClanSetMode(c *Client, raw []byte, dep deps) {
	var m clanSetModeMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "clan_set_mode", err)
		return
	}
	mode := world.ClanMode(m.Mode)
	if err := AuthzClanMode(c.PlayerID, mode, dep.world); err != nil {
		sendErr(c, "clan_set_mode", err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	if !dep.world.SetClanMode(p.ClanID, mode, c.PlayerID) {
		sendErr(c, "clan_set_mode", ErrNoClan)
		return
	}
	broadcastClanModeChanged(dep, p.ClanID, mode, c.PlayerID)
}

type clanRemoteMuteMsg struct {
	TargetID uint32 `json:"target_id"`
	Muted    bool   `json:"muted"`
	Scope    string `json:"scope"` // "clan" or "ally"
}

func handleClanRemoteMute(c *Client, raw []byte, dep deps) {
	var m clanRemoteMuteMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "clan_remote_mute", err)
		return
	}
	if err := AuthzRemoteMute(c.PlayerID, m.TargetID, m.Scope, dep.world); err != nil {
		sendErr(c, "clan_remote_mute", err)
		return
	}
	onClan := m.Scope == "clan"
	onAlly := m.Scope == "ally"
	if m.Muted {
		dep.world.AddRemoteMute(c.PlayerID, m.TargetID, onClan, onAlly)
	} else {
		dep.world.ClearRemoteMute(c.PlayerID, m.TargetID)
	}
	// Tell the target so their GUI shows the indicator (Regra 5:
	// "membro mutado vê na GUI um indicador").
	dep.reg.SendToPlayer(m.TargetID, map[string]interface{}{
		"type":    "notification",
		"subtype": "remotely_muted_by",
		"from":    c.PlayerID,
		"scope":   m.Scope,
		"muted":   m.Muted,
	})
	// Fanout fresh client_state to everyone in the clan (+ally if the
	// scope is ally) so every leader/sub sees the updated remote_muted
	// flag on the affected member's row.
	actor := dep.world.Player(c.PlayerID)
	if actor != nil {
		pids := dep.world.ClanMembers(actor.ClanID)
		if onAlly && actor.AllyID != 0 {
			pids = append(pids, dep.world.AllyMembers(actor.AllyID)...)
		}
		pushClientStateToGroup(dep, pids)
	}
}

type clanOptOutMsg struct {
	OptOut bool `json:"opt_out"`
}

func handleClanOptOut(c *Client, raw []byte, dep deps) {
	var m clanOptOutMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "clan_opt_out_unified", err)
		return
	}
	if err := AuthzLeaderOrSub(c.PlayerID, dep.world); err != nil {
		sendErr(c, "clan_opt_out_unified", err)
		return
	}
	p := dep.world.Player(c.PlayerID)
	dep.world.SetClanOptOut(p.ClanID, m.OptOut)
}

type ccGrantSpeakMsg struct {
	TargetID uint32 `json:"target_id"`
	Granted  bool   `json:"granted"`
}

func handleCCGrantSpeak(c *Client, raw []byte, dep deps) {
	var m ccGrantSpeakMsg
	if err := json.Unmarshal(raw, &m); err != nil {
		sendErr(c, "cc_grant_speak", err)
		return
	}
	if err := AuthzCCGrantSpeak(c.PlayerID, m.TargetID, dep.world); err != nil {
		sendErr(c, "cc_grant_speak", err)
		return
	}
	cc := dep.world.CommandChannel(c.PlayerID)
	if cc == nil {
		return
	}
	if !dep.world.SetCCSpeakPermission(cc.ID, m.TargetID, m.Granted) {
		sendErr(c, "cc_grant_speak", ErrTargetNotInCC)
		return
	}
	// Notify everyone in the CC so their tab reflects the new state.
	pushClientStateToGroup(dep, cc.MemberIDs)
}

// ----- broadcasts ----------------------------------------------------

func broadcastSubLeaderChanged(dep deps, clanID, target uint32, promoted bool) {
	frame := map[string]interface{}{
		"type":     "subleader_changed",
		"clan_id":  clanID,
		"target":   target,
		"promoted": promoted,
	}
	members := dep.world.ClanMembers(clanID)
	dep.reg.BroadcastToPlayers(members, frame)
	// State changed for every clan member (target's role flipped).
	pushClientStateToGroup(dep, members)
}

func broadcastClanModeChanged(dep deps, clanID uint32, mode world.ClanMode, by uint32) {
	frame := map[string]interface{}{
		"type":    "clan_mode_changed",
		"clan_id": clanID,
		"mode":    mode,
		"by":      by,
	}
	// Notify the clan and the alliance (so allies know about mode
	// changes that affect the unified channel).
	pids := dep.world.ClanMembers(clanID)
	if c := dep.world.Clan(clanID); c != nil {
		if p := dep.world.Player(by); p != nil && p.AllyID != 0 {
			pids = append(pids, dep.world.AllyMembers(p.AllyID)...)
		}
		_ = c
	}
	dep.reg.BroadcastToPlayers(pids, frame)
	pushClientStateToGroup(dep, pids)
	log.Printf("control: clan %d mode -> %d by %d", clanID, mode, by)
}
