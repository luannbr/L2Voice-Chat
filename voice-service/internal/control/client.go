package control

import (
	"sync"

	"github.com/gorilla/websocket"
)

// Client wraps a single authenticated WS connection with the per-conn
// write mutex required by gorilla/websocket (writes must not interleave).
type Client struct {
	SID      uint32
	PlayerID uint32
	conn     *websocket.Conn
	writeMu  sync.Mutex
}

// Send marshals v to JSON and writes it to the underlying conn.
// Safe for concurrent callers (write is serialized).
func (c *Client) Send(v interface{}) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return c.conn.WriteJSON(v)
}

// ConnReg holds the live WS clients keyed by session ID.
// Lookups happen on every clan event broadcast, so RLock-heavy reads.
type ConnReg struct {
	mu      sync.RWMutex
	clients map[uint32]*Client
	byPID   map[uint32]*Client // playerID -> client, for direct notifications
}

func NewConnReg() *ConnReg {
	return &ConnReg{
		clients: map[uint32]*Client{},
		byPID:   map[uint32]*Client{},
	}
}

func (r *ConnReg) Register(c *Client) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.clients[c.SID] = c
	if c.PlayerID != 0 {
		r.byPID[c.PlayerID] = c
	}
}

func (r *ConnReg) Unregister(sid uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.clients[sid]
	if !ok {
		return
	}
	delete(r.clients, sid)
	if c.PlayerID != 0 && r.byPID[c.PlayerID] == c {
		delete(r.byPID, c.PlayerID)
	}
}

// ClientByPlayer returns the Client for a given L2 character id, or nil.
func (r *ConnReg) ClientByPlayer(pid uint32) *Client {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.byPID[pid]
}

// SendToPlayer marshals v to the connection of `pid` if one is alive.
// Returns false silently if the player is offline (no error — clan
// broadcasts naturally include offline members).
func (r *ConnReg) SendToPlayer(pid uint32, v interface{}) bool {
	c := r.ClientByPlayer(pid)
	if c == nil {
		return false
	}
	_ = c.Send(v)
	return true
}

// BroadcastToPlayers sends v to every alive client in the given pid set.
func (r *ConnReg) BroadcastToPlayers(pids []uint32, v interface{}) {
	for _, pid := range pids {
		r.SendToPlayer(pid, v)
	}
}

// CloseByPlayer forces the WS connection for `pid` (if any) to close.
// Used when the bridge tells us the player just logged out / went to
// char-select — without this, the DLL would happily keep the link
// alive even though its session is gone.
func (r *ConnReg) CloseByPlayer(pid uint32) bool {
	r.mu.RLock()
	c := r.byPID[pid]
	r.mu.RUnlock()
	if c == nil {
		return false
	}
	// gorilla/websocket close: send a close control frame, then
	// shutter the underlying TCP socket. The handleWS read loop
	// returns error → defers fire → session is dropped.
	c.writeMu.Lock()
	_ = c.conn.Close()
	c.writeMu.Unlock()
	return true
}
