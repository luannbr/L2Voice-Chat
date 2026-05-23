package control

import (
	"errors"
	"testing"

	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// World fixture for authz tests. Smaller than the router's — just
// enough to validate role checks.
type aWorld struct{ w *world.WorldState }

func newA(t *testing.T) *aWorld { return &aWorld{w: world.NewWorldState()} }

func (a *aWorld) player(id, clan, ally uint32, leader bool) *aWorld {
	a.w.UpsertPlayer(id, clan, ally, 0, 0, leader)
	return a
}
func (a *aWorld) clan(id, leader uint32, subs ...uint32) *aWorld {
	a.w.UpsertClan(id, leader, false)
	for _, s := range subs {
		a.w.SetSubLeader(id, s, true)
	}
	return a
}

// Promote: only the leader can promote, target must be in same clan,
// can't promote self or the leader.
func TestAuthzPromoteSubLeader(t *testing.T) {
	a := newA(t).
		player(10, 100, 0, true).
		player(11, 100, 0, false).
		player(12, 200, 0, true).
		clan(100, 10).
		clan(200, 12)

	if err := AuthzPromoteSubLeader(10, 11, a.w); err != nil {
		t.Fatalf("leader promoting clan member should succeed: %v", err)
	}
	if err := AuthzPromoteSubLeader(11, 11, a.w); !errors.Is(err, ErrRequiresLeader) {
		t.Errorf("non-leader promote: want ErrRequiresLeader, got %v", err)
	}
	if err := AuthzPromoteSubLeader(10, 12, a.w); !errors.Is(err, ErrNotSameClan) {
		t.Errorf("cross-clan promote: want ErrNotSameClan, got %v", err)
	}
	if err := AuthzPromoteSubLeader(10, 10, a.w); !errors.Is(err, ErrCannotTargetSelf) {
		t.Errorf("self-promote: want ErrCannotTargetSelf, got %v", err)
	}
	if err := AuthzPromoteSubLeader(999, 11, a.w); !errors.Is(err, ErrUnknownPlayer) {
		t.Errorf("unknown actor: want ErrUnknownPlayer, got %v", err)
	}
}

// LeaderOrSub: leader yes, sub yes, plain member no, no-clan no.
func TestAuthzLeaderOrSub(t *testing.T) {
	a := newA(t).
		player(10, 100, 0, true).
		player(11, 100, 0, false).
		player(12, 100, 0, false).
		player(99, 0, 0, false).
		clan(100, 10, 11)

	if err := AuthzLeaderOrSub(10, a.w); err != nil {
		t.Errorf("leader: %v", err)
	}
	if err := AuthzLeaderOrSub(11, a.w); err != nil {
		t.Errorf("sub-leader: %v", err)
	}
	if err := AuthzLeaderOrSub(12, a.w); !errors.Is(err, ErrRequiresLeaderOrSub) {
		t.Errorf("member: want ErrRequiresLeaderOrSub, got %v", err)
	}
	if err := AuthzLeaderOrSub(99, a.w); !errors.Is(err, ErrNoClan) {
		t.Errorf("no-clan: want ErrNoClan, got %v", err)
	}
}

// RemoteMute: sub can't target leader or another sub.
func TestAuthzRemoteMute_SubCannotTouchLeaderOrSub(t *testing.T) {
	a := newA(t).
		player(10, 100, 0, true).
		player(11, 100, 0, false).
		player(12, 100, 0, false). // sub-leader
		player(13, 100, 0, false). // member
		clan(100, 10, 12)

	// Sub (11) targeting leader (10): rejected.
	// Wait, 11 isn't sub. Use 12 (the sub).
	if err := AuthzRemoteMute(12, 10, "clan", a.w); !errors.Is(err, ErrSubCannotTouchLeader) {
		t.Errorf("sub→leader: want ErrSubCannotTouchLeader, got %v", err)
	}
	// Sub→sub: also rejected. (Promote 11 too.)
	a.w.SetSubLeader(100, 11, true)
	if err := AuthzRemoteMute(12, 11, "clan", a.w); !errors.Is(err, ErrSubCannotTouchLeader) {
		t.Errorf("sub→sub: want ErrSubCannotTouchLeader, got %v", err)
	}
	// Sub→member: ok.
	if err := AuthzRemoteMute(12, 13, "clan", a.w); err != nil {
		t.Errorf("sub→member: %v", err)
	}
	// Leader→sub: ok.
	if err := AuthzRemoteMute(10, 12, "clan", a.w); err != nil {
		t.Errorf("leader→sub: %v", err)
	}
}

// RemoteMute: self target rejected.
func TestAuthzRemoteMute_NoSelf(t *testing.T) {
	a := newA(t).
		player(10, 100, 0, true).
		clan(100, 10)
	if err := AuthzRemoteMute(10, 10, "clan", a.w); !errors.Is(err, ErrCannotTargetSelf) {
		t.Errorf("self: want ErrCannotTargetSelf, got %v", err)
	}
}

// RemoteMute: ally scope requires both in same ally.
func TestAuthzRemoteMute_AllyScope(t *testing.T) {
	a := newA(t).
		player(10, 100, 555, true).
		player(20, 200, 555, false).
		player(99, 300, 0, false).
		clan(100, 10).
		clan(200, 20)
	if err := AuthzRemoteMute(10, 20, "ally", a.w); err != nil {
		t.Errorf("leader cross-clan-same-ally: %v", err)
	}
	if err := AuthzRemoteMute(10, 99, "ally", a.w); !errors.Is(err, ErrNotInGroup) {
		t.Errorf("target outside ally: want ErrNotInGroup, got %v", err)
	}
}

// Mode validation: only the 5 enumerated modes allowed.
func TestAuthzClanMode(t *testing.T) {
	a := newA(t).
		player(10, 100, 0, true).
		clan(100, 10)
	for _, m := range []world.ClanMode{world.ModeNone, world.ModePVP, world.ModeSiege, world.ModeBoss, world.ModeFarm} {
		if err := AuthzClanMode(10, m, a.w); err != nil {
			t.Errorf("valid mode %d rejected: %v", m, err)
		}
	}
	if err := AuthzClanMode(10, world.ClanMode(99), a.w); !errors.Is(err, ErrBadMode) {
		t.Errorf("bad mode: want ErrBadMode, got %v", err)
	}
}

func TestClampVolume(t *testing.T) {
	cases := []struct{ in, out float32 }{
		{-1, 0}, {0, 0}, {0.5, 0.5}, {2, 2}, {2.5, 2},
	}
	for _, c := range cases {
		if got := ClampVolume(c.in); got != c.out {
			t.Errorf("ClampVolume(%v)=%v want %v", c.in, got, c.out)
		}
	}
}

// CC grant-speak authz: only the CC leader, target must be in same CC.
func TestAuthzCCGrantSpeak(t *testing.T) {
	a := newA(t).
		player(100, 0, 0, false).
		player(200, 0, 0, false).
		player(300, 0, 0, false).
		player(999, 0, 0, false)
	a.w.UpsertCommandChannel(900, 100, []uint32{100, 200, 300})

	if err := AuthzCCGrantSpeak(100, 200, a.w); err != nil {
		t.Errorf("CC leader grant member: %v", err)
	}
	if err := AuthzCCGrantSpeak(200, 300, a.w); !errors.Is(err, ErrRequiresCCLeader) {
		t.Errorf("non-leader grant: want ErrRequiresCCLeader, got %v", err)
	}
	if err := AuthzCCGrantSpeak(100, 100, a.w); !errors.Is(err, ErrCannotTargetSelf) {
		t.Errorf("self: want ErrCannotTargetSelf, got %v", err)
	}
	if err := AuthzCCGrantSpeak(100, 999, a.w); !errors.Is(err, ErrTargetNotInCC) {
		t.Errorf("target outside CC: want ErrTargetNotInCC, got %v", err)
	}
	if err := AuthzCCGrantSpeak(999, 200, a.w); !errors.Is(err, ErrNotInCC) {
		t.Errorf("actor not in any CC: want ErrNotInCC, got %v", err)
	}
}

func TestValidChannel(t *testing.T) {
	for _, ch := range []world.Channel{world.ChProximity, world.ChParty, world.ChClan, world.ChAlly} {
		if err := ValidChannel(ch); err != nil {
			t.Errorf("ch %d should be valid: %v", ch, err)
		}
	}
	if err := ValidChannel(world.ChModeUnified); !errors.Is(err, ErrBadChannel) {
		t.Errorf("ChModeUnified must be server-only: want ErrBadChannel, got %v", err)
	}
}
