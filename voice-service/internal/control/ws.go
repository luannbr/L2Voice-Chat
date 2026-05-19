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
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/luannbr/l2voice/voice-service/internal/topology"
)

// CheckEndpoint is the URL of the L2J bridge /voice/check endpoint.
// Empty disables the check (stub-auth accepts any player_id). Set
// from main.go via SetCheckEndpoint.
var checkEndpoint string

// SetCheckEndpoint configures the L2J bridge URL. Called once at startup.
func SetCheckEndpoint(url string) { checkEndpoint = url }

// authMsg is the first message a client must send.
type authMsg struct {
	Type          string `json:"type"`
	Token         string `json:"token"`
	PlayerID      uint32 `json:"player_id"`
	ClientVersion string `json:"client_version,omitempty"`
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

	// Read auth as the first message; deadline so a non-talking
	// client can't sit on a socket forever.
	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
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

	// Identity check against the L2J bridge: is this player_id currently
	// online in L2World? If the bridge isn't configured (dev / no GS),
	// accept any player_id — useful for loopback and bench tests.
	if checkEndpoint != "" {
		ok, err := checkPlayerOnline(msg.PlayerID)
		if err != nil {
			log.Printf("control: /voice/check error for player=%d: %v", msg.PlayerID, err)
			_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "validate_error"})
			return
		}
		if !ok {
			log.Printf("control: %s rejected — player %d not online", r.RemoteAddr, msg.PlayerID)
			_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "player_not_online"})
			return
		}
	}

	sess := state.AllocSession(msg.PlayerID)
	defer state.Drop(sess.ID)

	udpEndpoint := udpEndpointFor(r)
	if err := conn.WriteJSON(authOk{
		Type:         "auth_ok",
		SessionID:    sess.ID,
		UDPEndpoint:  udpEndpoint,
		YourPlayerID: msg.PlayerID,
	}); err != nil {
		return
	}
	log.Printf("control: %s authed as session=%d player=%d (client=%s)",
		r.RemoteAddr, sess.ID, msg.PlayerID, msg.ClientVersion)

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

// checkPlayerOnline calls the L2J bridge to verify the player_id is
// currently in L2World. Short timeout — auth is on the hot path of
// a connect, the client is waiting.
func checkPlayerOnline(playerID uint32) (bool, error) {
	client := http.Client{Timeout: 2 * time.Second}
	url := fmt.Sprintf("%s?player_id=%d", checkEndpoint, playerID)
	resp, err := client.Get(url)
	if err != nil {
		return false, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return false, fmt.Errorf("status %d: %s", resp.StatusCode, body)
	}
	var out struct {
		Online   bool   `json:"online"`
		PlayerID uint32 `json:"player_id"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return false, err
	}
	return out.Online, nil
}
