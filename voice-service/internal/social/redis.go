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

// Subscribe runs the Redis SUBSCRIBE loop. Reconnects with backoff on
// drop. Returns only when ctx is cancelled.
//
// state (topology) carries network-session-level data (UDP addr,
// last-seen). worldState carries game-level data (clan/ally/party,
// positions, sub-leaders, etc.). Both are updated as relevant events
// arrive.
func Subscribe(ctx context.Context, cfg Config, state *topology.State,
	worldState *world.WorldState) error {
	if cfg.Channel == "" {
		cfg.Channel = "l2voice:events"
	}
	backoff := time.Second
	for {
		if err := ctx.Err(); err != nil {
			return nil
		}
		if err := runOnce(ctx, cfg, state, worldState); err != nil {
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
	worldState *world.WorldState) error {
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
			handleEvent([]byte(body), state, worldState)
		case "pmessage":
			if len(arr) >= 4 {
				body, _ := arr[3].(string)
				handleEvent([]byte(body), state, worldState)
			}
		}
	}
}

func handleEvent(raw []byte, state *topology.State, w *world.WorldState) {
	var ev Event
	if err := json.Unmarshal(raw, &ev); err != nil {
		log.Printf("social: malformed event: %v", err)
		return
	}
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
		// Ensure the Player record exists; upsert without disturbing
		// fields we don't know (use zeros for ally/party; subsequent
		// ally/party events will fill them in).
		existing := w.Player(ev.PlayerID)
		var allyID uint32
		var partyID uint64
		var instanceID uint32
		if existing != nil {
			allyID = existing.AllyID
			partyID = existing.PartyID
			instanceID = existing.InstanceID
		}
		w.UpsertPlayer(ev.PlayerID, p.ClanID, allyID, partyID, instanceID, p.IsLeader)

	case "ally_change":
		var p allyChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		existing := w.Player(ev.PlayerID)
		var clanID, instanceID uint32
		var partyID uint64
		var isLeader bool
		if existing != nil {
			clanID = existing.ClanID
			partyID = existing.PartyID
			instanceID = existing.InstanceID
			isLeader = existing.IsLeader
		}
		w.UpsertPlayer(ev.PlayerID, clanID, p.AllyID, partyID, instanceID, isLeader)

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
		w.UpsertPlayer(ev.PlayerID, clanID, allyID, p.PartyID, instanceID, isLeader)

	case "clan_leader_change":
		var p clanLeaderChangePayload
		if err := json.Unmarshal(ev.Data, &p); err != nil {
			return
		}
		w.UpsertClan(p.ClanID, p.LeaderID, false)

	case "player_logout":
		if sess != nil {
			state.Drop(sess.ID)
		}
		w.RemovePlayer(ev.PlayerID)
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
