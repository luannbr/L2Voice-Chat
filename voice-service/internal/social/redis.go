// Package social consumes player-state events from the L2J bridge
// over Redis pub/sub on channel "l2voice:events". The events drive
// the voice-service's authoritative view of player positions, party
// membership, clan, ally and instance transitions.
//
// Wire format and event list: see docs/protocol.md §5.
//
// The subscriber is intentionally minimal — no Redis client library
// dependency is mandatory. We dial the broker over plain TCP and
// speak the RESP3 SUBSCRIBE flow. For an MVP this avoids dragging
// in go-redis just for one PSUBSCRIBE.
package social

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"strconv"
	"time"

	"github.com/luannbr/l2voice/voice-service/internal/topology"
	"github.com/luannbr/l2voice/voice-service/internal/world"
)

// Config bundles the Redis connection settings.
type Config struct {
	Addr    string // host:port (e.g., "127.0.0.1:6379")
	Channel string // pub/sub channel; defaults to "l2voice:events"
}

// Event is the on-the-wire JSON envelope from l2j-bridge.
type Event struct {
	TS       int64           `json:"ts"`
	Event    string          `json:"event"`
	PlayerID uint32          `json:"player_id"`
	Data     json.RawMessage `json:"data"`
}

// positionPayload is the data{} body for "position" and the position
// subset of "player_login".
type positionPayload struct {
	X          float32 `json:"x"`
	Y          float32 `json:"y"`
	Z          float32 `json:"z"`
	InstanceID uint32  `json:"instance_id"`
}

type clanChangePayload struct {
	ClanID   uint32 `json:"clan_id"`
	IsLeader bool   `json:"is_leader"`
}

type allyChangePayload struct {
	AllyID uint32 `json:"ally_id"`
}

type partyChangePayload struct {
	PartyID uint64 `json:"party_id"`
}

type clanLeaderChangePayload struct {
	ClanID   uint32 `json:"clan_id"`
	LeaderID uint32 `json:"leader_id"`
}

// ccChangePayload — emitted by the bridge whenever a player's command
// channel changes (joined, left, leader rotated, dissolved). cc_id=0
// means the player is no longer in any CC.
type ccChangePayload struct {
	CCID      uint32   `json:"cc_id"`
	LeaderID  uint32   `json:"cc_leader_id"`
	MemberIDs []uint32 `json:"members"`
}

// OnPlayerStateChange is called by the subscriber after any event
// mutates a player's world view. Implementations push fresh
// client_state snapshots to the affected WS clients. The callback
// must be cheap: it's invoked synchronously from the Redis recv loop.
type OnPlayerStateChange func(playerID uint32)

// OnPlayerLogout fires once when the bridge announces the player has
// left the world (logged out, kicked, DC'd). Implementations should
// close any WS connection for this player so the DLL knows to suspend.
type OnPlayerLogout func(playerID uint32)

// Subscribe runs the Redis SUBSCRIBE loop. Reconnects with backoff on
// drop. Returns only when ctx is cancelled.
//
// state (topology) carries network-session-level data (UDP addr,
// last-seen). worldState carries game-level data (clan/ally/party,
// positions, sub-leaders, etc.). onStateChange (optional) is invoked
// after each world mutation so the control plane can nudge clients.
// onPlayerLogout (optional) fires once on player_logout events so the
// control plane can close the WS to that player.
func Subscribe(ctx context.Context, cfg Config, state *topology.State,
	worldState *world.WorldState, onStateChange OnPlayerStateChange,
	onPlayerLogout OnPlayerLogout) error {
	if cfg.Channel == "" {
		cfg.Channel = "l2voice:events"
	}
	backoff := time.Second
	for {
		if err := ctx.Err(); err != nil {
			return nil
		}
		if err := runOnce(ctx, cfg, state, worldState, onStateChange, onPlayerLogout); err != nil {
			log.Printf("social: redis subscribe error: %v (retry in %v)", err, backoff)
			select {
			case <-time.After(backoff):
			case <-ctx.Done():
				return nil
			}
			if backoff < 30*time.Second {
				backoff *= 2
			}
			continue
		}
		backoff = time.Second
	}
}

func runOnce(ctx context.Context, cfg Config, state *topology.State,
	worldState *world.WorldState, onStateChange OnPlayerStateChange,
	onPlayerLogout OnPlayerLogout) error {
	d := net.Dialer{Timeout: 5 * time.Second}
	conn, err := d.DialContext(ctx, "tcp", cfg.Addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	log.Printf("social: connected to redis %s, subscribing %q", cfg.Addr, cfg.Channel)

	go func() {
		<-ctx.Done()
		conn.SetReadDeadline(time.Unix(1, 0))
		conn.Close()
	}()

	// Issue SUBSCRIBE.
	cmd := fmt.Sprintf("*2\r\n$9\r\nSUBSCRIBE\r\n$%d\r\n%s\r\n",
		len(cfg.Channel), cfg.Channel)
	if _, err := conn.Write([]byte(cmd)); err != nil {
		return err
	}

	r := bufio.NewReaderSize(conn, 1<<15)
	for {
		// Each message is a RESP array; we want the third element of
		// "message"-type frames.
		arr, err := readRESPArray(r)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		if len(arr) < 3 {
			continue
		}
		kind, _ := arr[0].(string)
		switch kind {
		case "subscribe":
			// confirmation; ignore
		case "message":
			body, _ := arr[2].(string)
			handleEvent([]byte(body), state, worldState, onStateChange, onPlayerLogout)
		case "pmessage":
			if len(arr) >= 4 {
				body, _ := arr[3].(string)
				handleEvent([]byte(body), state, worldState, onStateChange, onPlayerLogout)
			}
		}
	}
}

// HandleEvent processes a single event envelope (same JSON shape as
// the Redis stream). Exported so the bridge WebSocket path can call
// it directly when Phase B is wired — same logic, two transports.
func HandleEvent(raw []byte, state *topology.State, w *world.WorldState,
	onStateChange OnPlayerStateChange, onPlayerLogout OnPlayerLogout) {
	handleEvent(raw, state, w, onStateChange, onPlayerLogout)
}

func handleEvent(raw []byte, state *topology.State, w *world.WorldState,
	onStateChange OnPlayerStateChange, onPlayerLogout OnPlayerLogout) {
	var ev Event
	if err := json.Unmarshal(raw, &ev); err != nil {
		log.Printf("social: malformed event: %v", err)
		return
	}
	// Diagnostic — log every non-position event so the operator can
	// see clan/ally/party/cc/login/logout traffic. Position spam is
	// suppressed (its arrival is implicit from "audio: sid=N rx=#K
	// speakerPos=true" in the audio package).
	if ev.Event != "position" {
		log.Printf("social: ev=%s player=%d", ev.Event, ev.PlayerID)
	}
	// Set of players whose client_state view changed and who should be
	// nudged. Always include the event subject. For group-membership
	// events we ALSO include the old + new group members so peers see
	// the join/leave in real time (was a missing fanout before).
	affected := map[uint32]struct{}{}
	if ev.PlayerID != 0 {
		affected[ev.PlayerID] = struct{}{}
	}
	defer func() {
		if onStateChange == nil {
			return
		}
		for id := range affected {
			onStateChange(id)
		}
	}()
	// World state tracks ALL online players (independent of voice
	// session), so we always update it. Voice-session updates only
	// happen when a session has been allocated.
	sess := state.LookupByPlayer(ev.PlayerID)

	switch ev.Event {
	case "position", "player_login":
		var p positionPayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		w.SetPlayerPosition(ev.PlayerID, p.X, p.Y, p.Z, p.InstanceID)
		if sess != nil {
			state.UpdatePosition(sess, p.X, p.Y, p.Z, p.InstanceID)
		}

	case "instance_change":
		var p positionPayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		if sess != nil {
			state.UpdatePosition(sess, sess.X, sess.Y, sess.Z, p.InstanceID)
		}
		// World: keep last-known X/Y/Z, replace instance.
		if pl := w.Player(ev.PlayerID); pl != nil {
			w.SetPlayerPosition(ev.PlayerID, pl.X, pl.Y, pl.Z, p.InstanceID)
		}

	case "clan_change":
		var p clanChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		// Fanout: old clan members (about to lose this player) +
		// new clan members (about to gain them).
		existing := w.Player(ev.PlayerID)
		var allyID uint32
		var partyID uint64
		var instanceID uint32
		var oldClanID uint32
		if existing != nil {
			allyID = existing.AllyID
			partyID = existing.PartyID
			instanceID = existing.InstanceID
			oldClanID = existing.ClanID
		}
		for _, id := range w.ClanMembers(oldClanID) {
			affected[id] = struct{}{}
		}
		w.UpsertPlayer(ev.PlayerID, p.ClanID, allyID, partyID, instanceID, p.IsLeader)
		for _, id := range w.ClanMembers(p.ClanID) {
			affected[id] = struct{}{}
		}

	case "ally_change":
		var p allyChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		existing := w.Player(ev.PlayerID)
		var clanID, instanceID uint32
		var partyID uint64
		var isLeader bool
		var oldAllyID uint32
		if existing != nil {
			clanID = existing.ClanID
			partyID = existing.PartyID
			instanceID = existing.InstanceID
			isLeader = existing.IsLeader
			oldAllyID = existing.AllyID
		}
		for _, id := range w.AllyMembers(oldAllyID) {
			affected[id] = struct{}{}
		}
		w.UpsertPlayer(ev.PlayerID, clanID, p.AllyID, partyID, instanceID, isLeader)
		for _, id := range w.AllyMembers(p.AllyID) {
			affected[id] = struct{}{}
		}

	case "party_change":
		var p partyChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		existing := w.Player(ev.PlayerID)
		var clanID, allyID, instanceID uint32
		var isLeader bool
		if existing != nil {
			clanID = existing.ClanID
			allyID = existing.AllyID
			instanceID = existing.InstanceID
			isLeader = existing.IsLeader
		}
		// Old party members (about to see this player leave).
		for _, id := range w.PartyMembers(ev.PlayerID) {
			affected[id] = struct{}{}
		}
		w.UpsertPlayer(ev.PlayerID, clanID, allyID, p.PartyID, instanceID, isLeader)
		// New party members (now see this player join).
		for _, id := range w.PartyMembers(ev.PlayerID) {
			affected[id] = struct{}{}
		}

	case "clan_leader_change":
		var p clanLeaderChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		w.UpsertClan(p.ClanID, p.LeaderID, false)
		for _, id := range w.ClanMembers(p.ClanID) {
			affected[id] = struct{}{}
		}

	case "cc_change":
		var p ccChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		// Capture the old CC's members before mutating so they get
		// a state push that no longer lists this player.
		if oldCC := w.CommandChannel(ev.PlayerID); oldCC != nil {
			for _, id := range oldCC.MemberIDs {
				affected[id] = struct{}{}
			}
		}
		if p.CCID == 0 {
			// Player left their CC; we can only dissolve from the
			// player side — if there were other members the bridge
			// will republish them with the survivors. As a safety
			// net here, drop this single player from any CC they're
			// currently tracked in.
			if cc := w.CommandChannel(ev.PlayerID); cc != nil {
				// Rebuild without this player (fresh slice to avoid
				// aliasing into UpsertCommandChannel which retains it).
				out := make([]uint32, 0, len(cc.MemberIDs))
				for _, id := range cc.MemberIDs {
					if id != ev.PlayerID {
						out = append(out, id)
					}
				}
				if len(out) == 0 {
					w.DissolveCommandChannel(cc.ID)
				} else {
					w.UpsertCommandChannel(cc.ID, cc.LeaderID, out)
				}
			}
		} else {
			w.UpsertCommandChannel(p.CCID, p.LeaderID, p.MemberIDs)
			for _, id := range p.MemberIDs {
				affected[id] = struct{}{}
			}
		}

	case "player_logout":
		if sess != nil {
			state.Drop(sess.ID)
		}
		w.RemovePlayer(ev.PlayerID)
		// Force-close any open WS for this player. Without this, the
		// DLL stays connected with a now-invalid session and the
		// overlay keeps showing as if the player were still in-world.
		if onPlayerLogout != nil {
			onPlayerLogout(ev.PlayerID)
		}
	}
}

// ---- minimal RESP3 parser ------------------------------------------
//
// Only what we need for SUBSCRIBE: simple strings, bulk strings,
// integers, arrays. Returns []interface{} for arrays, string for
// bulk/simple, int64 for integers.

func readRESPArray(r *bufio.Reader) ([]interface{}, error) {
	first, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	switch first {
	case '*':
		n, err := readInt(r)
		if err != nil {
			return nil, err
		}
		if n < 0 {
			return nil, nil
		}
		out := make([]interface{}, n)
		for i := int64(0); i < n; i++ {
			elem, err := readValue(r)
			if err != nil {
				return nil, err
			}
			out[i] = elem
		}
		return out, nil
	default:
		return nil, fmt.Errorf("expected array, got %q", first)
	}
}

func readValue(r *bufio.Reader) (interface{}, error) {
	tag, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	switch tag {
	case '+':
		return readLine(r)
	case '-':
		s, err := readLine(r)
		if err != nil {
			return nil, err
		}
		return nil, errors.New(s)
	case ':':
		return readInt(r)
	case '$':
		n, err := readInt(r)
		if err != nil {
			return nil, err
		}
		if n < 0 {
			return "", nil
		}
		buf := make([]byte, n+2)
		if _, err := readFull(r, buf); err != nil {
			return nil, err
		}
		return string(buf[:n]), nil
	case '*':
		n, err := readInt(r)
		if err != nil {
			return nil, err
		}
		out := make([]interface{}, n)
		for i := int64(0); i < n; i++ {
			v, err := readValue(r)
			if err != nil {
				return nil, err
			}
			out[i] = v
		}
		return out, nil
	default:
		return nil, fmt.Errorf("unsupported RESP tag %q", tag)
	}
}

func readInt(r *bufio.Reader) (int64, error) {
	s, err := readLine(r)
	if err != nil {
		return 0, err
	}
	return strconv.ParseInt(s, 10, 64)
}

func readLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	if len(line) < 2 {
		return "", fmt.Errorf("short line")
	}
	return line[:len(line)-2], nil
}

func readFull(r *bufio.Reader, buf []byte) (int, error) {
	got := 0
	for got < len(buf) {
		n, err := r.Read(buf[got:])
		got += n
		if err != nil {
			return got, err
		}
	}
	return got, nil
}
