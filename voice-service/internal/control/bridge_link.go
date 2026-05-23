// Outbound bridge link (POC, Phase 5+ multi-VPS prep).
//
// The L2J bridge runs inside the GameServer on the operator's home
// network, behind a NAT. The voice-server may run on any number of
// VPSes in different regions. To avoid port-forwarding the bridge
// HTTP callback and to enable multi-VPS scaling, the bridge opens an
// outbound persistent WebSocket to each voice-server instead of being
// called back.
//
// This file implements the SERVER side of that link: a `/bridge`
// WebSocket endpoint on the existing control port (:17667). For the
// POC scope we only exchange:
//
//   bridge → server: {"type":"hello", "gs":"<name>", "v":"<bridge_version>"}
//   server → bridge: {"type":"welcome"}
//   either side:     {"type":"ping","t":<ms>} / {"type":"pong","t":<echo>}
//
// On a verified POC, Phase A migrates the existing whoami / name /
// group HTTP callbacks to RPC frames over this same link, and Phase B
// migrates the Redis event stream to be pushed over it.
package control

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

// BridgeLink represents one connected bridge. Phase A adds RPC: the
// voice-server can ask the bridge for `whoami` (resolve player_id from
// TCP source ports) and `name` (player_id → character name) over the
// same WebSocket, eliminating the inbound HTTP callbacks.
type BridgeLink struct {
	conn    *websocket.Conn
	writeMu sync.Mutex
	GS      string // GS identity from hello message
	Version string // bridge version string

	// RPC machinery — outstanding requests keyed by correlation id.
	// The read loop pulls responses off this map and signals waiters.
	rpcMu      sync.Mutex
	rpcPending map[uint64]chan rpcReply
	nextRpcID  atomic.Uint64
}

type rpcReply struct {
	playerID uint32 // for rpc_whoami_resp
	name     string // for rpc_name_resp
	ok       bool
}

// Send writes a JSON object to the bridge. Safe for concurrent callers.
func (b *BridgeLink) Send(v interface{}) error {
	b.writeMu.Lock()
	defer b.writeMu.Unlock()
	return b.conn.WriteJSON(v)
}

// rpcCall issues an RPC request with the given JSON payload, waits up
// to `timeout` for a reply matching the correlation id, and returns
// the rpcReply. Caller is responsible for setting the request type.
func (b *BridgeLink) rpcCall(payload map[string]interface{}, timeout time.Duration) (rpcReply, error) {
	id := b.nextRpcID.Add(1)
	payload["id"] = id
	ch := make(chan rpcReply, 1)
	b.rpcMu.Lock()
	if b.rpcPending == nil {
		b.rpcPending = map[uint64]chan rpcReply{}
	}
	b.rpcPending[id] = ch
	b.rpcMu.Unlock()
	defer func() {
		b.rpcMu.Lock()
		delete(b.rpcPending, id)
		b.rpcMu.Unlock()
	}()
	if err := b.Send(payload); err != nil {
		return rpcReply{}, err
	}
	select {
	case r := <-ch:
		return r, nil
	case <-time.After(timeout):
		return rpcReply{}, errors.New("bridge rpc timeout")
	}
}

// RPCWhoami asks the bridge to resolve a player_id from the given
// client IP and candidate TCP source ports. Returns 0 if no match.
func (b *BridgeLink) RPCWhoami(ip string, ports []uint16) (uint32, error) {
	// Marshal ports as an integer array for the Java side.
	portInts := make([]int, len(ports))
	for i, p := range ports {
		portInts[i] = int(p)
	}
	r, err := b.rpcCall(map[string]interface{}{
		"type":  "rpc_whoami",
		"ip":    ip,
		"ports": portInts,
	}, 3*time.Second)
	if err != nil {
		return 0, err
	}
	return r.playerID, nil
}

// RPCName asks the bridge for the character name of an online player.
// Returns "" if the player isn't online (or the bridge can't resolve).
func (b *BridgeLink) RPCName(playerID uint32) (string, error) {
	r, err := b.rpcCall(map[string]interface{}{
		"type":      "rpc_name",
		"player_id": playerID,
	}, 2*time.Second)
	if err != nil {
		return "", err
	}
	return r.name, nil
}

// Active bridges by their remote-address string. Phase A will key by
// `GS` identity once bridges send a stable name in hello.
var (
	bridgesMu sync.RWMutex
	bridges   = map[string]*BridgeLink{}
)

// FirstActiveBridge returns any currently-connected bridge link, or
// nil. For single-GS deployments (the common case) the iteration
// returns the only bridge. Phase C will add per-region selection.
func FirstActiveBridge() *BridgeLink {
	bridgesMu.RLock()
	defer bridgesMu.RUnlock()
	for _, b := range bridges {
		return b
	}
	return nil
}

// BridgeEventHandler is invoked once per inbound `{"type":"event",
// "payload":{...}}` frame on the /bridge WS. The payload is the
// Redis-style envelope: `{"event":"position","player_id":N,"data":...}`.
// main.go provides an implementation that fans through
// social.HandleEvent into world/topology state.
type BridgeEventHandler func(payload []byte)

// Set globally by Serve(); read by HandleBridgeWS when it sees an
// event frame. nil = bridge events are ignored (Redis still works).
var bridgeEventHandler BridgeEventHandler

// SetBridgeEventHandler wires the event sink. main.go calls this
// once at startup before clients connect.
func SetBridgeEventHandler(h BridgeEventHandler) {
	bridgeEventHandler = h
}

// HandleBridgeWS is the http.HandlerFunc for `/bridge`. Mounted on
// the same server as `/ws` so we don't open another firewall port.
func HandleBridgeWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("bridge: upgrade error: %v", err)
		return
	}
	defer conn.Close()
	remote := r.RemoteAddr
	log.Printf("bridge: connected from %s (UA=%q)", remote, r.Header.Get("User-Agent"))

	link := &BridgeLink{conn: conn}
	bridgesMu.Lock()
	bridges[remote] = link
	bridgesMu.Unlock()
	defer func() {
		bridgesMu.Lock()
		delete(bridges, remote)
		bridgesMu.Unlock()
		log.Printf("bridge: disconnected %s", remote)
	}()

	// Read loop. Bridge initiates messages; we respond and occasionally
	// emit pings of our own so both directions are exercised.
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		for range t.C {
			now := time.Now().UnixMilli()
			if err := link.Send(map[string]interface{}{
				"type": "ping", "t": now, "from": "server",
			}); err != nil {
				return
			}
		}
	}()

	for {
		_, raw, err := conn.ReadMessage()
		if err != nil {
			return
		}
		var hdr struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(raw, &hdr); err != nil {
			continue
		}
		switch hdr.Type {
		case "hello":
			var m struct {
				GS string `json:"gs"`
				V  string `json:"v"`
			}
			_ = json.Unmarshal(raw, &m)
			link.GS = m.GS
			link.Version = m.V
			log.Printf("bridge: hello gs=%q v=%q from %s", m.GS, m.V, remote)
			_ = link.Send(map[string]interface{}{"type": "welcome"})
		case "ping":
			var m struct {
				T int64 `json:"t"`
			}
			_ = json.Unmarshal(raw, &m)
			_ = link.Send(map[string]interface{}{"type": "pong", "t": m.T, "from": "server"})
		case "pong":
			var m struct {
				T int64 `json:"t"`
			}
			_ = json.Unmarshal(raw, &m)
			rtt := time.Now().UnixMilli() - m.T
			log.Printf("bridge: rtt=%dms (%s)", rtt, remote)
		case "rpc_whoami_resp":
			var m struct {
				ID       uint64 `json:"id"`
				PlayerID uint32 `json:"player_id"`
			}
			_ = json.Unmarshal(raw, &m)
			link.rpcMu.Lock()
			ch, ok := link.rpcPending[m.ID]
			link.rpcMu.Unlock()
			if ok {
				select {
				case ch <- rpcReply{playerID: m.PlayerID, ok: true}:
				default:
				}
			}
		case "rpc_name_resp":
			var m struct {
				ID   uint64 `json:"id"`
				Name string `json:"name"`
			}
			_ = json.Unmarshal(raw, &m)
			link.rpcMu.Lock()
			ch, ok := link.rpcPending[m.ID]
			link.rpcMu.Unlock()
			if ok {
				select {
				case ch <- rpcReply{name: m.Name, ok: true}:
				default:
				}
			}
		case "event":
			// Phase B: bridge forwards player events over this link
			// instead of (or in addition to) Redis. Body shape:
			//   {"type":"event","payload":{"event":"position","player_id":N,"data":{...}}}
			// Payload format matches the Redis envelope verbatim, so
			// social.HandleEvent treats both transports identically.
			if bridgeEventHandler != nil {
				var env struct {
					Payload json.RawMessage `json:"payload"`
				}
				if err := json.Unmarshal(raw, &env); err == nil && len(env.Payload) > 0 {
					bridgeEventHandler([]byte(env.Payload))
				}
			}
		default:
			log.Printf("bridge: unknown msg type=%q from %s", hdr.Type, remote)
		}
	}
}
