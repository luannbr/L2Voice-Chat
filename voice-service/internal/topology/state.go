// Package topology holds the in-memory map of active sessions.
// Both the UDP audio router and the WebSocket control plane consult
// it. Reads dominate (every audio packet does a grid lookup), so we
// optimize for read concurrency with RWMutex.
package topology

import (
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// Session is one connected voice client.
type Session struct {
	ID         uint32       // server-allocated, 1-based
	PlayerID   uint32       // L2 character object id (from auth)
	UDPAddr    *net.UDPAddr // resolved from first audio packet, mutable after NAT rebind
	X, Y, Z    float32      // latest position reported on a proximity packet
	InstanceID uint32       // 0 = main world
	LastSeen   time.Time
}

// State is the global session table.
type State struct {
	mu       sync.RWMutex
	nextID   atomic.Uint32
	bySID    map[uint32]*Session
	byUDP    map[string]*Session // key = udpAddr.String()
	byPlayer map[uint32]*Session
}

// NewState returns an empty topology state.
func NewState() *State {
	return &State{
		bySID:    make(map[uint32]*Session),
		byUDP:    make(map[string]*Session),
		byPlayer: make(map[uint32]*Session),
	}
}

// AllocSession creates a new Session with a fresh sequential ID.
// Called by the WS control plane on successful auth.
func (s *State) AllocSession(playerID uint32) *Session {
	id := s.nextID.Add(1)
	sess := &Session{
		ID:       id,
		PlayerID: playerID,
		LastSeen: time.Now(),
	}
	s.mu.Lock()
	s.bySID[id] = sess
	if playerID != 0 {
		s.byPlayer[playerID] = sess
	}
	s.mu.Unlock()
	return sess
}

// LookupBySID returns the session (or nil) for a given session ID.
func (s *State) LookupBySID(id uint32) *Session {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.bySID[id]
}

// RememberUDP records the UDP source address for a session. Called
// from the UDP path on the first packet (or after a NAT rebind).
func (s *State) RememberUDP(sess *Session, addr *net.UDPAddr) {
	s.mu.Lock()
	if sess.UDPAddr != nil {
		delete(s.byUDP, sess.UDPAddr.String())
	}
	sess.UDPAddr = addr
	s.byUDP[addr.String()] = sess
	s.mu.Unlock()
}

// UpdatePosition records the latest position from a proximity packet.
// Called every audio frame (~50 Hz per active speaker).
func (s *State) UpdatePosition(sess *Session, x, y, z float32, instance uint32) {
	s.mu.Lock()
	sess.X, sess.Y, sess.Z = x, y, z
	sess.InstanceID = instance
	sess.LastSeen = time.Now()
	s.mu.Unlock()
}

// ProximityNeighbors returns all OTHER sessions within `radius` cm of
// the speaker's current position, in the same instance. Caller holds
// no locks.
//
// MVP implementation: linear scan. Acceptable up to ~500 active
// sessions globally. Above that, switch to a uniform grid keyed by
// (instance, floor(x/cellSize), floor(y/cellSize)).
func (s *State) ProximityNeighbors(speaker *Session, radius float32) []*Session {
	radiusSq := radius * radius
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*Session, 0, 16)
	for _, sess := range s.bySID {
		if sess == speaker {
			continue
		}
		if sess.InstanceID != speaker.InstanceID {
			continue
		}
		dx := sess.X - speaker.X
		dy := sess.Y - speaker.Y
		dz := sess.Z - speaker.Z
		if dx*dx+dy*dy+dz*dz <= radiusSq {
			out = append(out, sess)
		}
	}
	return out
}

// Drop removes a session (called on WS disconnect or stale UDP).
func (s *State) Drop(id uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	sess, ok := s.bySID[id]
	if !ok {
		return
	}
	delete(s.bySID, id)
	if sess.UDPAddr != nil {
		delete(s.byUDP, sess.UDPAddr.String())
	}
	if sess.PlayerID != 0 {
		delete(s.byPlayer, sess.PlayerID)
	}
}

// Count returns the current number of active sessions (for metrics).
func (s *State) Count() int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.bySID)
}
