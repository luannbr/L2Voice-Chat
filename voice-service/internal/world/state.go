// Package world holds the authoritative voice-side view of the L2
// game state: players, clans, alliances, parties, plus the
// voice-only overlay (sub-leaders, remote mutes, active clan modes,
// per-player prefs).
//
// Source of truth split:
//   - clan_id, ally_id, party_id, IsLeader, position
//        → published by the L2J bridge over Redis pub/sub
//   - sub-leaders, channel toggles, per-player volume/mute,
//     whisper mode, clan mode, remote mutes
//        → set by clients over the WS control channel; ephemeral
//          in voice-service memory (no DB). Leader's voice.ini
//          carries the sub-leader list across leader sessions.
//
// All mutations go through methods on WorldState, which holds a
// sync.RWMutex. Readers (the router) take RLock; mutators take Lock.
// The router runs on the UDP goroutine at ~50 Hz per active speaker,
// so reads dominate.
package world

import (
	"sync"
	"time"
)

// Channel ids — match the wire protocol.
type Channel uint8

const (
	ChProximity   Channel = 0
	ChParty       Channel = 1
	ChClan        Channel = 2
	ChAlly        Channel = 3
	ChModeUnified Channel = 5 // synthetic egress channel when a clan mode is active
)

// Role within a clan, as far as the voice system is concerned.
type Role uint8

const (
	RoleMember    Role = 0
	RoleSubLeader Role = 1 // our own concept, not L2J's pledge rank
	RoleLeader    Role = 2 // matches L2Clan.getLeaderId
)

// ClanMode — the "operational" mode set by the leader/sub-leader.
// While any non-None mode is active, clan + ally collapse into one
// isolated channel.
type ClanMode uint8

const (
	ModeNone  ClanMode = 0
	ModePVP   ClanMode = 1
	ModeSiege ClanMode = 2
	ModeBoss  ClanMode = 3
	ModeFarm  ClanMode = 4
)

// PlayerPrefs is the per-player preference set sent from the DLL via
// WS messages. Updates arrive incrementally; the voice-service caches
// the latest values per player. Defaults: all channels enabled, all
// volumes at 1.0, no per-player mutes, whisper off.
type PlayerPrefs struct {
	ChannelEnabled  [4]bool            // indexed by Channel (0..3)
	ChannelVolume   [4]float32         // 0..1
	PerPlayerMute   map[uint32]bool    // sender id → muted by this listener
	PerPlayerVolume map[uint32]float32 // sender id → 0..2 multiplier
	WhisperMode     uint8              // 0=off, 1=ptt, 2=always
	WhisperKey      int                // VK
}

// NewPlayerPrefs returns prefs with sensible defaults.
func NewPlayerPrefs() *PlayerPrefs {
	p := &PlayerPrefs{
		PerPlayerMute:   map[uint32]bool{},
		PerPlayerVolume: map[uint32]float32{},
		WhisperMode:     0,
	}
	for i := 0; i < 4; i++ {
		p.ChannelEnabled[i] = true
		p.ChannelVolume[i] = 1.0
	}
	return p
}

// Player is the voice-service's view of an in-world character. The
// fields that come from L2J (ClanID, AllyID, PartyID, IsLeader,
// position) are updated by the Redis subscriber; the rest is set by
// WS messages.
type Player struct {
	ID         uint32 // L2J ObjectId
	SessionID  uint32 // 0 until voice WS auths
	ClanID     uint32
	AllyID     uint32 // 0 = no alliance
	PartyID    uint64 // 0 = solo
	InstanceID uint32
	X, Y, Z    float32
	IsLeader   bool         // L2Clan.getLeaderId == this.ID
	Prefs      *PlayerPrefs // never nil after AddPlayer
	LastSeen   time.Time
}

// Clan tracks voice-side clan state. SubLeaders is unlimited per
// project convention. Mode is the currently-active operational mode
// set by leader or any sub-leader.
type Clan struct {
	ID                    uint32
	LeaderID              uint32 // from L2J
	SubLeaders            map[uint32]struct{}
	Mode                  ClanMode
	ModeSetBy             uint32 // player id who activated the mode
	IsAllyLeader          bool   // this clan is the alliance's founder clan
	OptedOutOfUnifiedMode bool   // member of an alliance, but using own mode
}

// Alliance tracks ally membership. Mode of the alliance is the mode
// of the LeaderClan (per user's design choice).
type Alliance struct {
	ID           uint32
	LeaderClanID uint32   // L2J: clan that founded the alliance
	MemberClans  []uint32 // includes LeaderClanID
}

// RemoteMute is a session-scoped mute applied by a clan leader or
// sub-leader to a clan/ally member. Lives until either party
// disconnects.
type RemoteMute struct {
	MuterID  uint32
	TargetID uint32
	OnClan   bool
	OnAlly   bool
	SetAt    time.Time
}

// WorldState is the centralized container. Lookups are O(1) via maps.
// Routing is O(group size).
type WorldState struct {
	mu          sync.RWMutex
	players     map[uint32]*Player
	clans       map[uint32]*Clan
	alliances   map[uint32]*Alliance
	parties     map[uint64]map[uint32]struct{} // partyID -> set of memberIDs
	remoteMutes []*RemoteMute
}

// NewWorldState returns an empty world.
func NewWorldState() *WorldState {
	return &WorldState{
		players:   map[uint32]*Player{},
		clans:     map[uint32]*Clan{},
		alliances: map[uint32]*Alliance{},
		parties:   map[uint64]map[uint32]struct{}{},
	}
}

// ---- Player lifecycle ----

func (w *WorldState) UpsertPlayer(id uint32, clanID, allyID uint32,
	partyID uint64, instanceID uint32, isLeader bool) *Player {
	w.mu.Lock()
	defer w.mu.Unlock()
	p, ok := w.players[id]
	if !ok {
		p = &Player{ID: id, Prefs: NewPlayerPrefs()}
		w.players[id] = p
	}
	w.movePartyLocked(p, partyID)
	p.ClanID = clanID
	p.AllyID = allyID
	p.InstanceID = instanceID
	p.IsLeader = isLeader
	p.LastSeen = time.Now()
	return p
}

func (w *WorldState) SetPlayerSession(id, sid uint32) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if p, ok := w.players[id]; ok {
		p.SessionID = sid
	}
}

func (w *WorldState) SetPlayerPosition(id uint32, x, y, z float32, instanceID uint32) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if p, ok := w.players[id]; ok {
		p.X, p.Y, p.Z = x, y, z
		p.InstanceID = instanceID
		p.LastSeen = time.Now()
	}
}

// RemovePlayer purges the player from everywhere — party membership,
// sub-leader lists, remote mutes. Called on player_logout.
func (w *WorldState) RemovePlayer(id uint32) {
	w.mu.Lock()
	defer w.mu.Unlock()
	p, ok := w.players[id]
	if !ok {
		return
	}
	w.movePartyLocked(p, 0)
	delete(w.players, id)
	// Drop sub-leader status everywhere this player was promoted.
	for _, c := range w.clans {
		delete(c.SubLeaders, id)
		// If the disconnecting player set the mode, clear it.
		if c.ModeSetBy == id {
			c.Mode = ModeNone
			c.ModeSetBy = 0
		}
	}
	// Drop any remote mutes involving this player.
	out := w.remoteMutes[:0]
	for _, m := range w.remoteMutes {
		if m.MuterID != id && m.TargetID != id {
			out = append(out, m)
		}
	}
	w.remoteMutes = out
}

// movePartyLocked moves p between party id buckets. Pass 0 to remove
// from any party. Caller must hold the write lock.
func (w *WorldState) movePartyLocked(p *Player, newPartyID uint64) {
	if p.PartyID == newPartyID {
		return
	}
	if p.PartyID != 0 {
		if set, ok := w.parties[p.PartyID]; ok {
			delete(set, p.ID)
			if len(set) == 0 {
				delete(w.parties, p.PartyID)
			}
		}
	}
	p.PartyID = newPartyID
	if newPartyID != 0 {
		set, ok := w.parties[newPartyID]
		if !ok {
			set = map[uint32]struct{}{}
			w.parties[newPartyID] = set
		}
		set[p.ID] = struct{}{}
	}
}

// ---- Clan / Alliance state ----

func (w *WorldState) UpsertClan(id, leaderID uint32, isAllyLeader bool) *Clan {
	w.mu.Lock()
	defer w.mu.Unlock()
	c, ok := w.clans[id]
	if !ok {
		c = &Clan{ID: id, SubLeaders: map[uint32]struct{}{}}
		w.clans[id] = c
	}
	c.LeaderID = leaderID
	c.IsAllyLeader = isAllyLeader
	return c
}

func (w *WorldState) UpsertAlliance(id, leaderClanID uint32, members []uint32) {
	w.mu.Lock()
	defer w.mu.Unlock()
	a, ok := w.alliances[id]
	if !ok {
		a = &Alliance{ID: id}
		w.alliances[id] = a
	}
	a.LeaderClanID = leaderClanID
	a.MemberClans = append(a.MemberClans[:0], members...)
}

// SetSubLeader promotes (true) or demotes (false) `targetID` in clan
// `clanID`. Returns true on success, false if the clan or target
// isn't visible. Authorization is the caller's job.
func (w *WorldState) SetSubLeader(clanID, targetID uint32, promote bool) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	c, ok := w.clans[clanID]
	if !ok {
		return false
	}
	if promote {
		c.SubLeaders[targetID] = struct{}{}
	} else {
		delete(c.SubLeaders, targetID)
	}
	return true
}

func (w *WorldState) SetClanMode(clanID uint32, mode ClanMode, setBy uint32) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	c, ok := w.clans[clanID]
	if !ok {
		return false
	}
	c.Mode = mode
	if mode == ModeNone {
		c.ModeSetBy = 0
	} else {
		c.ModeSetBy = setBy
	}
	return true
}

func (w *WorldState) SetClanOptOut(clanID uint32, optOut bool) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	c, ok := w.clans[clanID]
	if !ok {
		return false
	}
	c.OptedOutOfUnifiedMode = optOut
	return true
}

// ---- Remote mutes ----

func (w *WorldState) AddRemoteMute(muter, target uint32, onClan, onAlly bool) {
	w.mu.Lock()
	defer w.mu.Unlock()
	// Drop any pre-existing entry for the same (muter, target) pair.
	out := w.remoteMutes[:0]
	for _, m := range w.remoteMutes {
		if m.MuterID == muter && m.TargetID == target {
			continue
		}
		out = append(out, m)
	}
	out = append(out, &RemoteMute{
		MuterID: muter, TargetID: target,
		OnClan: onClan, OnAlly: onAlly,
		SetAt: time.Now(),
	})
	w.remoteMutes = out
}

func (w *WorldState) ClearRemoteMute(muter, target uint32) {
	w.mu.Lock()
	defer w.mu.Unlock()
	out := w.remoteMutes[:0]
	for _, m := range w.remoteMutes {
		if m.MuterID == muter && m.TargetID == target {
			continue
		}
		out = append(out, m)
	}
	w.remoteMutes = out
}

// IsRemotelyMuted returns true if at least one active remote mute
// covers (target, scope). Scope = "clan" or "ally". Reads only.
func (w *WorldState) IsRemotelyMuted(target uint32, scope string) bool {
	w.mu.RLock()
	defer w.mu.RUnlock()
	for _, m := range w.remoteMutes {
		if m.TargetID != target {
			continue
		}
		switch scope {
		case "clan":
			if m.OnClan {
				return true
			}
		case "ally":
			if m.OnAlly {
				return true
			}
		}
	}
	return false
}

// ---- Read-only accessors used by the router ----

func (w *WorldState) Player(id uint32) *Player {
	w.mu.RLock()
	defer w.mu.RUnlock()
	return w.players[id]
}

func (w *WorldState) Clan(id uint32) *Clan {
	w.mu.RLock()
	defer w.mu.RUnlock()
	return w.clans[id]
}

func (w *WorldState) Alliance(id uint32) *Alliance {
	w.mu.RLock()
	defer w.mu.RUnlock()
	return w.alliances[id]
}

// RoleOf returns the player's role within their own clan.
// RoleMember if no clan or not flagged.
func (w *WorldState) RoleOf(id uint32) Role {
	w.mu.RLock()
	defer w.mu.RUnlock()
	p, ok := w.players[id]
	if !ok || p.ClanID == 0 {
		return RoleMember
	}
	if p.IsLeader {
		return RoleLeader
	}
	c, ok := w.clans[p.ClanID]
	if !ok {
		return RoleMember
	}
	if _, sub := c.SubLeaders[id]; sub {
		return RoleSubLeader
	}
	return RoleMember
}

// PartyMembers returns the ids in the speaker's party (including the
// speaker). Empty if no party.
func (w *WorldState) PartyMembers(playerID uint32) []uint32 {
	w.mu.RLock()
	defer w.mu.RUnlock()
	p, ok := w.players[playerID]
	if !ok || p.PartyID == 0 {
		return nil
	}
	set := w.parties[p.PartyID]
	out := make([]uint32, 0, len(set))
	for id := range set {
		out = append(out, id)
	}
	return out
}

// ClanMembers returns the ids in the speaker's clan.
func (w *WorldState) ClanMembers(clanID uint32) []uint32 {
	w.mu.RLock()
	defer w.mu.RUnlock()
	if clanID == 0 {
		return nil
	}
	out := []uint32{}
	for id, p := range w.players {
		if p.ClanID == clanID {
			out = append(out, id)
		}
	}
	return out
}

// AllyMembers returns the ids of every player in any clan of the
// given ally.
func (w *WorldState) AllyMembers(allyID uint32) []uint32 {
	w.mu.RLock()
	defer w.mu.RUnlock()
	if allyID == 0 {
		return nil
	}
	out := []uint32{}
	for id, p := range w.players {
		if p.AllyID == allyID {
			out = append(out, id)
		}
	}
	return out
}

// SetPrefs replaces the entire PlayerPrefs blob (sent from DLL on
// auth or full re-sync).
func (w *WorldState) SetPrefs(playerID uint32, prefs *PlayerPrefs) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if p, ok := w.players[playerID]; ok {
		p.Prefs = prefs
	}
}
