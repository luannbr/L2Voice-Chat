package router

import (
	"math"
	"sort"
	"testing"

	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// ----- helpers -------------------------------------------------------

// fixture builds a small world by hand. Methods are chainable.
type fixture struct {
	t *testing.T
	w *world.WorldState
}

func newFixture(t *testing.T) *fixture {
	return &fixture{t: t, w: world.NewWorldState()}
}

// addPlayer registers a player with the given ids. clanID=0 means no
// clan; allyID=0 means no alliance; partyID=0 means solo.
func (f *fixture) addPlayer(id, clanID, allyID uint32, partyID uint64,
	instance uint32, isLeader bool) *fixture {
	f.w.UpsertPlayer(id, clanID, allyID, partyID, instance, isLeader)
	return f
}

func (f *fixture) at(id uint32, x, y, z float32) *fixture {
	p := f.w.Player(id)
	if p == nil {
		f.t.Fatalf("at(%d): player not registered", id)
	}
	f.w.SetPlayerPosition(id, x, y, z, p.InstanceID)
	return f
}

func (f *fixture) clan(id, leaderID uint32, subleaders ...uint32) *fixture {
	f.w.UpsertClan(id, leaderID, false)
	for _, s := range subleaders {
		f.w.SetSubLeader(id, s, true)
	}
	return f
}

func (f *fixture) mode(clanID uint32, m world.ClanMode, setBy uint32) *fixture {
	f.w.SetClanMode(clanID, m, setBy)
	return f
}

func (f *fixture) optOut(clanID uint32, opt bool) *fixture {
	f.w.SetClanOptOut(clanID, opt)
	return f
}

func (f *fixture) disableCh(playerID uint32, ch world.Channel) *fixture {
	p := f.w.Player(playerID)
	if p == nil {
		f.t.Fatalf("disableCh: player %d missing", playerID)
	}
	p.Prefs.ChannelEnabled[ch] = false
	return f
}

func (f *fixture) chVol(playerID uint32, ch world.Channel, v float32) *fixture {
	p := f.w.Player(playerID)
	p.Prefs.ChannelVolume[ch] = v
	return f
}

func (f *fixture) mutePlayer(listener, sender uint32) *fixture {
	p := f.w.Player(listener)
	p.Prefs.PerPlayerMute[sender] = true
	return f
}

func (f *fixture) volPlayer(listener, sender uint32, v float32) *fixture {
	p := f.w.Player(listener)
	p.Prefs.PerPlayerVolume[sender] = v
	return f
}

func (f *fixture) remoteMute(muter, target uint32, clan, ally bool) *fixture {
	f.w.AddRemoteMute(muter, target, clan, ally)
	return f
}

func recipients(decs []Decision) []uint32 {
	out := make([]uint32, 0, len(decs))
	for _, d := range decs {
		out = append(out, d.RecipientID)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

func find(decs []Decision, id uint32) (Decision, bool) {
	for _, d := range decs {
		if d.RecipientID == id {
			return d, true
		}
	}
	return Decision{}, false
}

func eqVol(a, b float32) bool {
	return math.Abs(float64(a-b)) < 1e-4
}

// ----- CLAN channel tests --------------------------------------------

// Member speaks in CLAN normal -> members with CLAN enabled receive.
func TestClan_MemberToEnabledMembers(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 100, 0, 0, 0, false). // sender, no override
		addPlayer(2, 100, 0, 0, 0, false). // peer, enabled
		addPlayer(3, 100, 0, 0, 0, false). // peer, disabled CLAN
		addPlayer(4, 200, 0, 0, 0, false). // other clan
		clan(100, 0).
		disableCh(3, world.ChClan)
	decs := Route(Packet{SenderID: 1, Channel: world.ChClan}, f.w, DefaultConfig())
	got := recipients(decs)
	if len(got) != 1 || got[0] != 2 {
		t.Fatalf("want [2], got %v", got)
	}
	if d, _ := find(decs, 2); d.ForceAudible {
		t.Fatalf("non-leader should NOT force audible")
	}
}

// Leader speaks in CLAN -> members with CLAN disabled also receive
// with ForceAudible=true.
func TestClan_LeaderOverridesDisabledToggle(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true). // leader
		addPlayer(11, 100, 0, 0, 0, false).
		addPlayer(12, 100, 0, 0, 0, false). // CLAN disabled
		clan(100, 10).
		disableCh(12, world.ChClan)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	d11, ok11 := find(decs, 11)
	d12, ok12 := find(decs, 12)
	if !ok11 || !ok12 {
		t.Fatalf("want both 11 and 12, got %v", recipients(decs))
	}
	if d11.ForceAudible {
		t.Errorf("11 has CLAN enabled, ForceAudible should be false")
	}
	if !d12.ForceAudible {
		t.Errorf("12 has CLAN disabled but leader overrides, ForceAudible must be true")
	}
}

// Sub-leader same as leader: override applies.
func TestClan_SubLeaderOverridesDisabledToggle(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true). // leader
		addPlayer(11, 100, 0, 0, 0, false).
		addPlayer(12, 100, 0, 0, 0, false). // CLAN disabled
		clan(100, 10, 11).
		disableCh(12, world.ChClan)
	decs := Route(Packet{SenderID: 11, Channel: world.ChClan}, f.w, DefaultConfig())
	if d, ok := find(decs, 12); !ok || !d.ForceAudible {
		t.Fatalf("sub-leader must override Disabled for 12, got %+v ok=%v", d, ok)
	}
}

// Leader speaks in CLAN, member muted the leader -> NOT received.
func TestClan_PerPlayerMuteBeatsLeaderOverride(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).
		addPlayer(11, 100, 0, 0, 0, false).
		clan(100, 10).
		mutePlayer(11, 10) // 11 muted leader 10
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("muted listener should not receive, got %+v", decs)
	}
}

// Remote-muted speaker in CLAN -> empty.
func TestClan_RemoteMuteSilencesSender(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).
		addPlayer(11, 100, 0, 0, 0, false).
		addPlayer(12, 100, 0, 0, 0, false).
		clan(100, 10).
		remoteMute(10, 12, true, false)
	decs := Route(Packet{SenderID: 12, Channel: world.ChClan}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("remote-muted-on-clan sender must produce no decisions, got %v", recipients(decs))
	}
}

// Sender with no clan, ChClan -> empty (nothing to route to).
func TestClan_NoClan(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 0, 0, 0, 0, false).
		addPlayer(2, 0, 0, 0, 0, false)
	decs := Route(Packet{SenderID: 1, Channel: world.ChClan}, f.w, DefaultConfig())
	if decs != nil && len(decs) != 0 {
		t.Fatalf("no clan -> no recipients, got %v", recipients(decs))
	}
}

// Volume math: ChannelVolume[ch] * PerPlayerVolume.
func TestClan_VolumeMath(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).
		addPlayer(11, 100, 0, 0, 0, false).
		clan(100, 10).
		chVol(11, world.ChClan, 0.5).
		volPlayer(11, 10, 2.0)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	d, ok := find(decs, 11)
	if !ok {
		t.Fatalf("11 must receive")
	}
	if !eqVol(d.Volume, 1.0) {
		t.Fatalf("want vol=1.0 (0.5*2.0), got %f", d.Volume)
	}
}

// ----- ALLY channel tests --------------------------------------------

// Leader speaks in ALLY -> all clans in ally hear, override applies.
func TestAlly_LeaderOverridesAcrossClans(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true). // leader of clan 100, in ally 555
		addPlayer(20, 200, 555, 0, 0, false).
		addPlayer(30, 200, 555, 0, 0, false). // ALLY disabled
		addPlayer(40, 300, 0, 0, 0, false).   // other clan, NO ally
		clan(100, 10).
		clan(200, 20).
		disableCh(30, world.ChAlly)
	decs := Route(Packet{SenderID: 10, Channel: world.ChAlly}, f.w, DefaultConfig())
	got := recipients(decs)
	wantSet := map[uint32]bool{20: true, 30: true}
	for _, id := range got {
		if !wantSet[id] {
			t.Errorf("unexpected recipient %d", id)
		}
	}
	for id := range wantSet {
		if _, ok := find(decs, id); !ok {
			t.Errorf("missing recipient %d (want=%v got=%v)", id, wantSet, got)
		}
	}
	if d, _ := find(decs, 30); !d.ForceAudible {
		t.Errorf("30 had ALLY disabled — leader override must set ForceAudible")
	}
}

// Remote mute on ally silences ALLY packet.
func TestAlly_RemoteMuteSilencesSender(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).
		addPlayer(20, 200, 555, 0, 0, false).
		addPlayer(11, 100, 555, 0, 0, false).
		clan(100, 10).
		clan(200, 20).
		remoteMute(10, 11, false, true) // muted on ally only
	decs := Route(Packet{SenderID: 11, Channel: world.ChAlly}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("ally-remote-muted sender produces no decisions, got %v", recipients(decs))
	}
}

// ----- PARTY channel tests -------------------------------------------

// Party is flat: no override, no remote-mute scoping.
// Remote-muted speaker still talks in party.
func TestParty_RemoteMuteNotInScope(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 7777, 0, true).
		addPlayer(11, 100, 0, 7777, 0, false).
		addPlayer(12, 100, 0, 7777, 0, false).
		clan(100, 10).
		remoteMute(10, 11, true, true)
	decs := Route(Packet{SenderID: 11, Channel: world.ChParty}, f.w, DefaultConfig())
	got := recipients(decs)
	if len(got) != 2 || got[0] != 10 || got[1] != 12 {
		t.Fatalf("party must ignore remote mute (Regra 5); want [10 12], got %v", got)
	}
}

// Party respects Disabled toggle (no override).
func TestParty_DisabledRespected(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 7777, 0, true).
		addPlayer(11, 100, 0, 7777, 0, false).
		clan(100, 10).
		disableCh(11, world.ChParty)
	decs := Route(Packet{SenderID: 10, Channel: world.ChParty}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("party disabled on listener -> no delivery (no leader override on party), got %v",
			recipients(decs))
	}
}

// Solo speaker on PARTY -> empty.
func TestParty_Solo(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 0, 0, 0, 0, false)
	decs := Route(Packet{SenderID: 1, Channel: world.ChParty}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("solo: no recipients, got %v", recipients(decs))
	}
}

// ----- PROXIMITY tests -----------------------------------------------

// Same instance, within radius -> received with attenuation.
func TestProximity_InRange(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 0, 0, 0, 0, false).at(1, 0, 0, 0).
		addPlayer(2, 0, 0, 0, 0, false).at(2, 500, 0, 0)
	decs := Route(Packet{
		SenderID: 1, Channel: world.ChProximity,
		InstanceID: 0, X: 0, Y: 0, Z: 0,
	}, f.w, DefaultConfig())
	d, ok := find(decs, 2)
	if !ok {
		t.Fatalf("2 within range must receive, got %+v", decs)
	}
	wantAtten := 1.0 - 500.0/1500.0
	if !eqVol(d.Volume, float32(wantAtten)) {
		t.Fatalf("attenuation: want %.4f got %.4f", wantAtten, d.Volume)
	}
}

// Out of range -> excluded.
func TestProximity_OutOfRange(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 0, 0, 0, 0, false).at(1, 0, 0, 0).
		addPlayer(2, 0, 0, 0, 0, false).at(2, 5000, 0, 0)
	decs := Route(Packet{SenderID: 1, Channel: world.ChProximity, X: 0, Y: 0, Z: 0}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("out of range, no delivery, got %v", decs)
	}
}

// Different instance -> excluded even at same coords.
func TestProximity_DifferentInstance(t *testing.T) {
	f := newFixture(t).
		addPlayer(1, 0, 0, 0, 0, false).at(1, 0, 0, 0).
		addPlayer(2, 0, 0, 0, 42, false).at(2, 0, 0, 0)
	decs := Route(Packet{SenderID: 1, Channel: world.ChProximity, InstanceID: 0}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("different instance must not receive, got %v", recipients(decs))
	}
}

// ----- MODE tests ----------------------------------------------------

// Mode active: CLAN ingress -> ModeUnified egress, both clan and ally
// receive, ForceAudible always true even when CLAN/ALLY disabled.
func TestMode_UnifiesClanAndAlly_AlwaysAudible(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).
		addPlayer(11, 100, 555, 0, 0, false). // CLAN disabled
		addPlayer(20, 200, 555, 0, 0, false). // ALLY disabled
		addPlayer(99, 300, 0, 0, 0, false).   // unrelated
		clan(100, 10).
		clan(200, 20).
		mode(100, world.ModeSiege, 10).
		disableCh(11, world.ChClan).
		disableCh(20, world.ChAlly)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	for _, id := range []uint32{11, 20} {
		d, ok := find(decs, id)
		if !ok {
			t.Fatalf("mode must reach %d", id)
		}
		if d.EgressChan != world.ChModeUnified {
			t.Errorf("recipient %d expected ChModeUnified egress, got %d", id, d.EgressChan)
		}
		if !d.ForceAudible {
			t.Errorf("recipient %d expected ForceAudible=true under mode", id)
		}
	}
	if _, ok := find(decs, 99); ok {
		t.Errorf("non-clan/non-ally player must NOT receive mode audio")
	}
}

// Mode active: PROXIMITY of the speaker still works for nearby
// non-members (mode does not steal proximity).
func TestMode_ProximityUnaffectedForNonMembers(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).at(10, 0, 0, 0).
		addPlayer(99, 300, 0, 0, 0, false).at(99, 200, 0, 0). // nearby non-member
		clan(100, 10).
		mode(100, world.ModeSiege, 10)
	// Speaker presses PROXIMITY PTT.
	decs := Route(Packet{
		SenderID: 10, Channel: world.ChProximity,
		InstanceID: 0, X: 0, Y: 0, Z: 0,
	}, f.w, DefaultConfig())
	d, ok := find(decs, 99)
	if !ok {
		t.Fatalf("mode active but proximity must still deliver to nearby non-member, got %v",
			recipients(decs))
	}
	if d.EgressChan != world.ChProximity {
		t.Errorf("mode shouldn't repaint proximity egress, got %d", d.EgressChan)
	}
}

// Inverse: a non-member speaks in proximity nearby a mode-active
// member -> the member STILL receives the proximity audio (mode
// isolation applies only to OUTBOUND clan/ally; the member can still
// hear the world around them).
//
// Prompt.txt §matrix doesn't explicitly forbid this, and Regra 8 says
// proximity is independent. Keeping permissive.
func TestMode_NonMemberProximityReachesMember(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).at(10, 0, 0, 0).
		addPlayer(99, 300, 0, 0, 0, false).at(99, 200, 0, 0).
		clan(100, 10).
		mode(100, world.ModeSiege, 10)
	decs := Route(Packet{
		SenderID: 99, Channel: world.ChProximity,
		InstanceID: 0, X: 200, Y: 0, Z: 0,
	}, f.w, DefaultConfig())
	if _, ok := find(decs, 10); !ok {
		t.Fatalf("non-member proximity to mode-active member should still deliver, got %v",
			recipients(decs))
	}
}

// Mode + opt-out: clan with OptedOutOfUnifiedMode=true is NOT under
// mode despite c.Mode != None.
func TestMode_OptOutTurnsOffUnified(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).
		addPlayer(11, 100, 555, 0, 0, false).
		clan(100, 10).
		mode(100, world.ModeSiege, 10).
		optOut(100, true).
		disableCh(11, world.ChClan)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	// Should behave like normal CLAN with leader override.
	d, ok := find(decs, 11)
	if !ok {
		t.Fatalf("opted-out clan: behave like normal CLAN, leader override -> 11 receives")
	}
	if d.EgressChan != world.ChClan {
		t.Errorf("opt-out: expected ChClan egress, got %d", d.EgressChan)
	}
}

// Mode + per-player mute on sender by a clan member -> that member is
// skipped (per-player mute always wins).
func TestMode_PerPlayerMuteHonored(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).
		addPlayer(11, 100, 0, 0, 0, false).
		clan(100, 10).
		mode(100, world.ModePVP, 10).
		mutePlayer(11, 10)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	if _, ok := find(decs, 11); ok {
		t.Fatalf("per-player mute must skip listener even under mode")
	}
}

// ----- WHISPER tests -------------------------------------------------

// Whisper active on leader speaking in PROXIMITY -> rewritten as CLAN
// override (not proximity).
func TestWhisper_LeaderForcedToClan(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).at(10, 0, 0, 0).
		addPlayer(11, 100, 0, 0, 0, false).at(11, 999999, 0, 0). // far away
		addPlayer(12, 100, 0, 0, 0, false).at(12, 999999, 0, 0).
		clan(100, 10).
		disableCh(12, world.ChClan)
	decs := Route(Packet{
		SenderID:      10,
		Channel:       world.ChProximity, // ingress is proximity
		InstanceID:    0,
		X:             0, Y: 0, Z: 0,
		WhisperActive: true,
	}, f.w, DefaultConfig())
	got := recipients(decs)
	if len(got) != 2 {
		t.Fatalf("whisper: clan members must receive regardless of position, got %v", got)
	}
	if d, _ := find(decs, 12); !d.ForceAudible {
		t.Fatalf("whisper rewrites to clan with override, but 12 didn't get ForceAudible")
	}
	for _, d := range decs {
		if d.EgressChan != world.ChClan {
			t.Errorf("whisper egress must be ChClan, got %d", d.EgressChan)
		}
	}
}

// Whisper from non-leader -> ignored, falls back to normal routing.
func TestWhisper_NonLeaderIgnored(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 0, 0, 0, true).
		addPlayer(11, 100, 0, 0, 0, false). // not leader, not sub
		clan(100, 10)
	decs := Route(Packet{
		SenderID:      11,
		Channel:       world.ChParty, // no party, normal routing returns nil
		WhisperActive: true,
	}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("non-leader whisper falls back; with no party, expect empty, got %v", decs)
	}
}

// ----- COMMAND CHANNEL tests -----------------------------------------

// Builds a CC with the given leader and members in a fresh world.
func (f *fixture) cc(ccID, leader uint32, members ...uint32) *fixture {
	all := append([]uint32{}, members...)
	all = append(all, leader)
	f.w.UpsertCommandChannel(ccID, leader, all)
	return f
}

// Leader can always speak in CC.
func TestCC_LeaderCanSpeak(t *testing.T) {
	f := newFixture(t).
		addPlayer(100, 0, 0, 7, 0, false).
		addPlayer(200, 0, 0, 8, 0, false).
		cc(900, 100, 200)
	decs := Route(Packet{SenderID: 100, Channel: world.ChCC}, f.w, DefaultConfig())
	if len(decs) != 1 || decs[0].RecipientID != 200 {
		t.Fatalf("leader→CC: want recipient [200], got %v", recipients(decs))
	}
	if !decs[0].ForceAudible {
		t.Errorf("CC packets should be ForceAudible")
	}
	if decs[0].EgressChan != world.ChCC {
		t.Errorf("expected ChCC egress, got %d", decs[0].EgressChan)
	}
}

// Non-permitted member can NOT speak in CC.
func TestCC_NonPermittedMemberCannotSpeak(t *testing.T) {
	f := newFixture(t).
		addPlayer(100, 0, 0, 7, 0, false).
		addPlayer(200, 0, 0, 8, 0, false).
		cc(900, 100, 200)
	decs := Route(Packet{SenderID: 200, Channel: world.ChCC}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("non-permitted speaker must produce no recipients, got %v", recipients(decs))
	}
}

// Granted member CAN speak; others receive ForceAudible.
func TestCC_GrantedMemberCanSpeak(t *testing.T) {
	f := newFixture(t).
		addPlayer(100, 0, 0, 7, 0, false).
		addPlayer(200, 0, 0, 8, 0, false).
		addPlayer(300, 0, 0, 9, 0, false).
		cc(900, 100, 200, 300)
	f.w.SetCCSpeakPermission(900, 200, true)
	decs := Route(Packet{SenderID: 200, Channel: world.ChCC}, f.w, DefaultConfig())
	got := recipients(decs)
	if len(got) != 2 || got[0] != 100 || got[1] != 300 {
		t.Fatalf("granted 200 -> [100,300], got %v", got)
	}
}

// Non-CC sender sending on ChCC -> empty.
func TestCC_NonMemberCannotSpeak(t *testing.T) {
	f := newFixture(t).
		addPlayer(100, 0, 0, 7, 0, false).
		addPlayer(999, 0, 0, 0, 0, false). // not in any CC
		cc(900, 100)
	decs := Route(Packet{SenderID: 999, Channel: world.ChCC}, f.w, DefaultConfig())
	if len(decs) != 0 {
		t.Fatalf("non-CC sender: no recipients, got %v", recipients(decs))
	}
}

// Per-player mute applies to CC too.
func TestCC_PerPlayerMuteHonored(t *testing.T) {
	f := newFixture(t).
		addPlayer(100, 0, 0, 7, 0, false).
		addPlayer(200, 0, 0, 8, 0, false).
		cc(900, 100, 200).
		mutePlayer(200, 100)
	decs := Route(Packet{SenderID: 100, Channel: world.ChCC}, f.w, DefaultConfig())
	if _, ok := find(decs, 200); ok {
		t.Fatalf("muted listener must not receive CC audio")
	}
}

// ----- Invariants ----------------------------------------------------

// PROXIMITY never receives ChModeUnified audio (Regra 6/8).
func TestInvariant_ModeUnifiedNeverOnProximityEgress(t *testing.T) {
	// Build a complex world and call Route many times with various
	// channels — assert no Decision combines ChProximity ingress with
	// ChModeUnified egress.
	f := newFixture(t).
		addPlayer(10, 100, 555, 100, 0, true).at(10, 0, 0, 0).
		addPlayer(11, 100, 555, 100, 0, false).at(11, 100, 0, 0).
		addPlayer(20, 200, 555, 200, 0, false).at(20, 200, 0, 0).
		addPlayer(99, 300, 0, 0, 0, false).at(99, 50, 0, 0).
		clan(100, 10).
		clan(200, 20).
		mode(100, world.ModeBoss, 10)
	for _, ch := range []world.Channel{world.ChProximity, world.ChParty, world.ChClan, world.ChAlly} {
		for _, sid := range []uint32{10, 11, 20, 99} {
			pkt := Packet{SenderID: sid, Channel: ch, InstanceID: 0, X: 0, Y: 0, Z: 0}
			for _, d := range Route(pkt, f.w, DefaultConfig()) {
				if ch == world.ChProximity && d.EgressChan == world.ChModeUnified {
					t.Errorf("invariant violated: proximity ingress -> mode unified egress (sender=%d)", sid)
				}
			}
		}
	}
}

// ForceAudible is never set when EgressChan toggle is ENABLED.
func TestInvariant_ForceAudibleOnlyWhenDisabled(t *testing.T) {
	f := newFixture(t).
		addPlayer(10, 100, 555, 0, 0, true).
		addPlayer(11, 100, 555, 0, 0, false). // enabled by default
		clan(100, 10)
	decs := Route(Packet{SenderID: 10, Channel: world.ChClan}, f.w, DefaultConfig())
	for _, d := range decs {
		p := f.w.Player(d.RecipientID)
		if d.ForceAudible && p.Prefs.ChannelEnabled[d.EgressChan] {
			t.Errorf("ForceAudible=true while %d has %d enabled — unnecessary",
				d.RecipientID, d.EgressChan)
		}
	}
}
