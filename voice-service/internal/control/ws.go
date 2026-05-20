// Package control implements the WebSocket control plane.
//
// MVP scope: stub authentication. Any client that sends a valid
// `auth` JSON gets a fresh session_id. Real token validation against
// the L2J HTTP endpoint comes later. Proximity is the only channel
// implemented at this layer (proximity is implicit on auth, no
// channel_join/leave needed for it).
package control

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/luannbr/l2voice/voice-service/internal/topology"
)

// WhoamiEndpoint is the URL of the L2J bridge /voice/whoami endpoint.
// Required for identity resolution: the voice-service has no way to
// know which player a WS connection belongs to without it.
var whoamiEndpoint string

// SetWhoamiEndpoint configures the L2J bridge URL. Called once at startup.
func SetWhoamiEndpoint(url string) { whoamiEndpoint = url }

// authMsg is the first message a client must send.
type authMsg struct {
	Type          string   `json:"type"`
	Ports         []uint16 `json:"ports"`           // local TCP source ports owned by the L2.exe process
	ClientVersion string   `json:"client_version,omitempty"`
}

// authOk is the success response.
type authOk struct {
	Type         string `json:"type"`
	SessionID    uint32 `json:"session_id"`
	UDPEndpoint  string `json:"udp_endpoint"`
	YourPlayerID uint32 `json:"your_player_id"`
}

type authFail struct {
	Type   string `json:"type"`
	Reason string `json:"reason"`
}

var upgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	// Accept any origin during dev; production should restrict.
	CheckOrigin: func(*http.Request) bool { return true },
}

// Serve starts the WebSocket control plane on listenAddr (e.g. ":17667").
// Returns when ctx is cancelled.
func Serve(ctx context.Context, listenAddr string, state *topology.State) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		handleWS(ctx, w, r, state)
	})
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("ok"))
	})

	srv := &http.Server{
		Addr:              listenAddr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	// Shut down server when ctx is cancelled.
	go func() {
		<-ctx.Done()
		shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutCtx)
	}()

	log.Printf("control: listening on %s (path /ws)", listenAddr)
	err := srv.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func handleWS(ctx context.Context, w http.ResponseWriter, r *http.Request, state *topology.State) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("control: upgrade error: %v", err)
		return
	}
	defer conn.Close()
	log.Printf("control: %s connected (UA=%q)", r.RemoteAddr, r.Header.Get("User-Agent"))

	// Read auth as the first message. The deadline is generous —
	// the DLL connects on L2.exe startup and only knows enough to
	// auth once the user has clicked through to in-world (the
	// GS TCP socket appears in the process's port table). That can
	// take a couple of minutes if the player lingers at character
	// selection. Don't tear down the WS in that window or
	// IXWebSocket reconnects with exponential backoff and ends up
	// in a 10–30 s "first-audio" delay after entering the world.
	conn.SetReadDeadline(time.Now().Add(5 * time.Minute))
	_, raw, err := conn.ReadMessage()
	if err != nil {
		log.Printf("control: %s read auth failed: %v", r.RemoteAddr, err)
		return
	}
	var msg authMsg
	if err := json.Unmarshal(raw, &msg); err != nil || msg.Type != "auth" {
		_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "bad_auth_msg"})
		return
	}

	if len(msg.Ports) == 0 {
		_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "no_ports"})
		return
	}

	// Extract client IP from the WS connection's remote addr.
	clientIP, _, _ := net.SplitHostPort(r.RemoteAddr)
	log.Printf("control: %s auth ports=%v clientIP=%q", r.RemoteAddr, msg.Ports, clientIP)

	// Resolve identity server-side via /voice/whoami.
	playerID, err := whoamiLookup(clientIP, msg.Ports)
	if err != nil {
		log.Printf("control: /voice/whoami error for %s: %v", r.RemoteAddr, err)
		_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "whoami_error"})
		return
	}
	if playerID == 0 {
		log.Printf("control: %s rejected — no matching online player (ports=%v)",
			r.RemoteAddr, msg.Ports)
		_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "player_not_resolved"})
		return
	}

	sess := state.AllocSession(playerID)
	defer state.Drop(sess.ID)
	msg.Ports = nil   // hint to GC; not needed beyond this point

	udpEndpoint := udpEndpointFor(r)
	if err := conn.WriteJSON(authOk{
		Type:         "auth_ok",
		SessionID:    sess.ID,
		UDPEndpoint:  udpEndpoint,
		YourPlayerID: playerID,
	}); err != nil {
		return
	}
	log.Printf("control: %s authed as session=%d player=%d (client=%s)",
		r.RemoteAddr, sess.ID, playerID, msg.ClientVersion)

	// Clear read deadline; rely on websocket ping/pong + topology
	// timeout for liveness checks.
	conn.SetReadDeadline(time.Time{})

	// Keep the connection alive. We don't process any further
	// messages yet (state_update, mute, channel_join/leave come
	// when we add party/clan/ally). Just block on read so the
	// connection's lifetime tracks the session's lifetime.
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		if _, _, err := conn.ReadMessage(); err != nil {
			log.Printf("control: session %d closed (%v)", sess.ID, err)
			return
		}
	}
}

// udpEndpointFor builds the udp_endpoint string returned in auth_ok.
// For now we hardcode by inspecting the HTTP host; production should
// have a config-driven public hostname.
var (
	udpEndpointOnce  sync.Once
	udpEndpointValue string
)

func udpEndpointFor(r *http.Request) string {
	udpEndpointOnce.Do(func() {
		// Strip port from Host, attach the well-known UDP port.
		// Hostname may be empty when listening on localhost in tests.
		host := r.Host
		if i := indexLastByte(host, ':'); i >= 0 {
			host = host[:i]
		}
		if host == "" {
			host = "127.0.0.1"
		}
		udpEndpointValue = host + ":17666"
	})
	return udpEndpointValue
}

func indexLastByte(s string, b byte) int {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == b {
			return i
		}
	}
	return -1
}

// whoamiLookup asks the L2J bridge "which player owns the TCP socket
// from clientIP with one of these source ports?" Returns 0 if no
// player matches.
func whoamiLookup(clientIP string, ports []uint16) (uint32, error) {
	if whoamiEndpoint == "" {
		return 0, fmt.Errorf("no whoami endpoint configured")
	}
	csv := make([]byte, 0, len(ports)*6)
	for i, p := range ports {
		if i > 0 {
			csv = append(csv, ',')
		}
		csv = strconv.AppendUint(csv, uint64(p), 10)
	}
	client := http.Client{Timeout: 2 * time.Second}
	url := fmt.Sprintf("%s?ip=%s&ports=%s", whoamiEndpoint, clientIP, csv)
	resp, err := client.Get(url)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return 0, fmt.Errorf("status %d: %s", resp.StatusCode, body)
	}
	var out struct {
		PlayerID uint32 `json:"player_id"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return 0, err
	}
	return out.PlayerID, nil
}
