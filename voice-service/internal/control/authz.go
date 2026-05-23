// Authorization rules for control-plane commands. Pure functions over
// world.WorldState — easy to unit-test, no I/O. All clan-impacting
// commands MUST go through one of these before mutating state.
//
// Server is the source of truth (Prompt.txt §Restrições): the client
// can SEND any payload, but the server validates here.
package control

import (
	"errors"
	"fmt"

	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// Errors returned by authz checks. The handler maps them to wire `err`
// frames; tests inspect them via errors.Is.
var (
	ErrUnknownPlayer   = errors.New("unknown player")
	ErrUnknownTarget   = errors.New("unknown target")
	ErrNoClan          = errors.New("player not in a clan")
	ErrNotSameClan    = errors.New("target not in same clan")
	ErrNotInGroup      = errors.New("target not in clan or ally")
	ErrRequiresLeader  = errors.New("requires clan leader")
	ErrRequiresLeaderOrSub = errors.New("requires leader or sub-leader")
	ErrCannotTargetSelf    = errors.New("cannot target self")
	ErrSubCannotTouchLeader = errors.New("sub-leader cannot remote-mute leader or another sub-leader")
	ErrBadChannel = errors.New("invalid channel id")
	ErrBadVolume  = errors.New("volume out of range")
	ErrBadMode    = errors.New("invalid clan mode")

	ErrNotInCC          = errors.New("player not in a command channel")
	ErrRequiresCCLeader = errors.New("requires command channel leader")
	ErrTargetNotInCC    = errors.New("target not in the command channel")
)

// AuthzPromoteSubLeader — only the clan leader can promote/demote.
// Target must be in the same clan, must not be the leader themselves.
//
// Implements: Prompt §Regra 2 ("Líder tem um painel ... onde adiciona/
// remove membros do clan como sub-líder").
func AuthzPromoteSubLeader(actor, target uint32, w *world.WorldState) error {
	a := w.Player(actor)
	if a == nil {
		return ErrUnknownPlayer
	}
	t := w.Player(target)
	if t == nil {
		return ErrUnknownTarget
	}
	if a.ClanID == 0 {
		return ErrNoClan
	}
	if a.ClanID != t.ClanID {
		return ErrNotSameClan
	}
	if !a.IsLeader {
		return ErrRequiresLeader
	}
	if actor == target {
		return ErrCannotTargetSelf
	}
	if t.IsLeader {
		return fmt.Errorf("target is the clan leader")
	}
	return nil
}

// AuthzLeaderOrSub — generic gate for commands available to leader or
// sub-leader (set mode, whisper config, etc.). Actor must have a clan.
func AuthzLeaderOrSub(actor uint32, w *world.WorldState) error {
	a := w.Player(actor)
	if a == nil {
		return ErrUnknownPlayer
	}
	if a.ClanID == 0 {
		return ErrNoClan
	}
	role := w.RoleOf(actor)
	if role != world.RoleLeader && role != world.RoleSubLeader {
		return ErrRequiresLeaderOrSub
	}
	return nil
}

// AuthzRemoteMute — Prompt §Regra 5.
// Leader or sub-leader can remote-mute; sub-leader CANNOT mute the
// leader or another sub-leader. Target must be in the same clan or
// (for ally scope) in the same alliance.
func AuthzRemoteMute(actor, target uint32, scope string, w *world.WorldState) error {
	if err := AuthzLeaderOrSub(actor, w); err != nil {
		return err
	}
	a := w.Player(actor)
	t := w.Player(target)
	if t == nil {
		return ErrUnknownTarget
	}
	if actor == target {
		return ErrCannotTargetSelf
	}
	switch scope {
	case "clan":
		if t.ClanID != a.ClanID {
			return ErrNotSameClan
		}
	case "ally":
		if a.AllyID == 0 || t.AllyID != a.AllyID {
			return ErrNotInGroup
		}
	default:
		return fmt.Errorf("scope must be \"clan\" or \"ally\"")
	}
	if w.RoleOf(actor) == world.RoleSubLeader {
		tr := w.RoleOf(target)
		if tr == world.RoleLeader || tr == world.RoleSubLeader {
			return ErrSubCannotTouchLeader
		}
	}
	return nil
}

// AuthzClanMode — set mode. Leader or sub-leader.
func AuthzClanMode(actor uint32, mode world.ClanMode, w *world.WorldState) error {
	if err := AuthzLeaderOrSub(actor, w); err != nil {
		return err
	}
	switch mode {
	case world.ModeNone, world.ModePVP, world.ModeSiege, world.ModeBoss, world.ModeFarm:
		return nil
	default:
		return ErrBadMode
	}
}

// AuthzWhisper — set whisper config. Leader or sub-leader.
func AuthzWhisper(actor uint32, mode uint8, w *world.WorldState) error {
	if err := AuthzLeaderOrSub(actor, w); err != nil {
		return err
	}
	if mode > 2 {
		return fmt.Errorf("whisper mode must be 0..2")
	}
	return nil
}

// AuthzCCGrantSpeak — Prompt §Comando Channel.
// Only the CC leader may grant/revoke speak permission, and only for
// members of their own CC. Self-target rejected.
func AuthzCCGrantSpeak(actor, target uint32, w *world.WorldState) error {
	if actor == target {
		return ErrCannotTargetSelf
	}
	cc := w.CommandChannel(actor)
	if cc == nil {
		return ErrNotInCC
	}
	if cc.LeaderID != actor {
		return ErrRequiresCCLeader
	}
	tcc := w.CommandChannel(target)
	if tcc == nil || tcc.ID != cc.ID {
		return ErrTargetNotInCC
	}
	return nil
}

// ValidChannel verifies the channel id is one of the four user-facing
// channels (PROXIMITY/PARTY/CLAN/ALLY). ChModeUnified is server-only.
// ChCC is a real channel but client doesn't yet send/set prefs for it
// (uses CLAN slot for volume per Phase D MVP); reject for prefs.
func ValidChannel(ch world.Channel) error {
	switch ch {
	case world.ChProximity, world.ChParty, world.ChClan, world.ChAlly:
		return nil
	}
	return ErrBadChannel
}

// ClampVolume clips v to [0, 2] (allowing 2x amplification per design).
func ClampVolume(v float32) float32 {
	if v < 0 {
		return 0
	}
	if v > 2 {
		return 2
	}
	return v
}
