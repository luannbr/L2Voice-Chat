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

// NameEndpoint is the URL of the L2J bridge /voice/name endpoint.
// Optional — when empty, name_query messages get an empty name back.
var nameEndpoint string

// SetWhoamiEndpoint configures the L2J bridge URL. Called once at startup.
func SetWhoamiEndpoint(url string) { whoamiEndpoint = url }

// SetNameEndpoint configures the bridge /voice/name URL.
func SetNameEndpoint(url string) { nameEndpoint = url }

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

	// First read the auth msg (with the candidate ports). The DLL
	// sends this once on WS Open. We then drive the whoami resolution
	// server-side, retrying on a timer until the player_id resolves
	// or the auth window (5 min) expires. This avoids needing the
	// DLL to re-send on auth_pending — most DLL builds won't, since
	// their port list is stable from "Login" click onward.
	clientIP, _, _ := net.SplitHostPort(r.RemoteAddr)
	authDeadline := time.Now().Add(5 * time.Minute)
	conn.SetReadDeadline(authDeadline)
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
	log.Printf("control: %s auth ports=%v clientIP=%q (resolving...)",
		r.RemoteAddr, msg.Ports, clientIP)

	// Now poll the bridge until it resolves the player.
	var playerID uint32
	for {
		if time.Now().After(authDeadline) {
			log.Printf("control: %s auth window exhausted (5 min)", r.RemoteAddr)
			_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "auth_timeout"})
			return
		}
		pid, lookupErr := whoamiLookup(clientIP, msg.Ports)
		if lookupErr == nil && pid != 0 {
			playerID = pid
			break
		}
		// Sleep before retrying. Cheap: one HTTP call to bridge per
		// pending client every 2 s, only while not-yet-resolved.
		select {
		case <-ctx.Done():
			return
		case <-time.After(2 * time.Second):
		}
	}
	log.Printf("control: %s auth resolved player=%d (ports=%v)",
		r.RemoteAddr, playerID, msg.Ports)

	sess := state.AllocSession(playerID)
	defer state.Drop(sess.ID)

	udpEndpoint := udpEndpointFor(r)
	if err := conn.WriteJSON(authOk{
		Type:         "auth_ok",
		SessionID:    sess.ID,
		UDPEndpoint:  udpEndpoint,
		YourPlayerID: playerID,
	}); err != nil {
		return
	}
	log.Printf("control: %s authed as session=%d player=%d",
		r.RemoteAddr, sess.ID, playerID)

	// Clear read deadline; rely on websocket ping/pong + topology
	// timeout for liveness checks.
	conn.SetReadDeadline(time.Time{})

	// Post-auth read loop. Currently handles one message type:
	//   name_query → look up the player_id of the requested sid via
	//   the topology table, then ask the bridge for the character
	//   name, reply with name_result. Everything else is ignored.
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		_, raw, err := conn.ReadMessage()
		if err != nil {
			log.Printf("control: session %d closed (%v)", sess.ID, err)
			return
		}
		var hdr struct {
			Type  string `json:"type"`
			SrcID uint32 `json:"src_id"`
		}
		if err := json.Unmarshal(raw, &hdr); err != nil {
			continue
		}
		if hdr.Type == "name_query" && hdr.SrcID != 0 {
			handleNameQuery(conn, state, hdr.SrcID)
		}
	}
}

// handleNameQuery resolves the character name for a sid the WS client
// is asking about and sends back name_result.
func handleNameQuery(conn *websocket.Conn, state *topology.State, srcID uint32) {
	target := state.LookupBySID(srcID)
	type nameResult struct {
		Type   string `json:"type"`
		SrcID  uint32 `json:"src_id"`
		Name   string `json:"name"`
	}
	out := nameResult{Type: "name_result", SrcID: srcID}
	if target == nil || target.PlayerID == 0 || nameEndpoint == "" {
		_ = conn.WriteJSON(out)
		return
	}
	url := fmt.Sprintf("%s?player_id=%d", nameEndpoint, target.PlayerID)
	client := http.Client{Timeout: 2 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		_ = conn.WriteJSON(out)
		return
	}
	defer resp.Body.Close()
	var body struct {
		PlayerID uint32 `json:"player_id"`
		Name     string `json:"name"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&body)
	out.Name = body.Name
	_ = conn.WriteJSON(out)
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
