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
	"github.com/luannbr/l2voice/voice-service/internal/world"
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
func Serve(ctx context.Context, listenAddr string, state *topology.State,
	worldState *world.WorldState, reg *ConnReg) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		handleWS(ctx, w, r, state, worldState, reg)
	})
	// POC: outbound link from the L2J bridge. Lives on the same port
	// so VPS firewall stays single-rule. See bridge_link.go.
	mux.HandleFunc("/bridge", HandleBridgeWS)
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

func handleWS(ctx context.Context, w http.ResponseWriter, r *http.Request,
	state *topology.State, worldState *world.WorldState, reg *ConnReg) {
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
	// Auth window — was 5 minutes, dropped to 30 s to bound the
	// rpc_whoami retry budget per connection. The DLL reconnects
	// roughly every 20 s due to IXWebSocket heartbeat handling;
	// if we keep a 5 min window, every reconnect spawns its own
	// 5-min retry loop and the bridge sees a constant flood of
	// whoami queries from orphaned auth-loops. 30 s is enough to
	// cover the latency of a player entering the world from char-
	// select right after the WS opens, while keeping orphan loops
	// short.
	authDeadline := time.Now().Add(30 * time.Second)
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

	// Poll the bridge until it resolves the player, with exponential
	// backoff (2s → 4s → 8s → max 8s). Coupled with the 30s auth
	// window, that yields at most ~4 RPCs per pending connection.
	var playerID uint32
	backoff := 2 * time.Second
	for {
		if time.Now().After(authDeadline) {
			log.Printf("control: %s auth window exhausted (30s)", r.RemoteAddr)
			_ = conn.WriteJSON(authFail{Type: "auth_fail", Reason: "auth_timeout"})
			return
		}
		pid, lookupErr := whoamiLookup(clientIP, msg.Ports)
		if lookupErr == nil && pid != 0 {
			playerID = pid
			break
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		if backoff < 8*time.Second {
			backoff *= 2
		}
	}
	log.Printf("control: %s auth resolved player=%d (ports=%v)",
		r.RemoteAddr, playerID, msg.Ports)

	sess := state.AllocSession(playerID)
	defer state.Drop(sess.ID)

	// Register the player in the world (if not yet seen via Redis
	// events). This guarantees Prefs exists before the first command.
	if worldState.Player(playerID) == nil {
		worldState.UpsertPlayer(playerID, 0, 0, 0, 0, false)
	}
	worldState.SetPlayerSession(playerID, sess.ID)

	client := &Client{SID: sess.ID, PlayerID: playerID, conn: conn}
	reg.Register(client)
	defer reg.Unregister(sess.ID)

	udpEndpoint := udpEndpointFor(r)
	if err := client.Send(authOk{
		Type:         "auth_ok",
		SessionID:    sess.ID,
		UDPEndpoint:  udpEndpoint,
		YourPlayerID: playerID,
	}); err != nil {
		return
	}
	// Push initial world snapshot so the DLL can populate tab member
	// lists, leader panel visibility, mode banner, and CC tab.
	initFrame := buildClientState(playerID, worldState, state)
	log.Printf("control: auth-time client_state for player=%d clan=%d ally=%d party=%d cc=%d members=%d/%d/%d/%d",
		playerID, initFrame.ClanID, initFrame.AllyID, initFrame.PartyID, initFrame.CCID,
		len(initFrame.PartyMembers), len(initFrame.ClanMembers),
		len(initFrame.AllyMembers), len(initFrame.CCMembers))
	_ = client.Send(initFrame)
	log.Printf("control: %s authed as session=%d player=%d",
		r.RemoteAddr, sess.ID, playerID)

	// Clear read deadline; rely on websocket ping/pong + topology
	// timeout for liveness checks.
	conn.SetReadDeadline(time.Time{})

	dep := deps{world: worldState, topo: state, reg: reg}

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
		if !dispatch(client, raw, dep) {
			// Unknown / malformed type — drop silently. (Logging every
			// unparsed frame would be too noisy at scale.)
		}
	}
}

// handlePlayerNameQuery resolves the character name for an L2 player
// object-id and replies with player_name_result. Unlike handleNameQuery
// (which keys by audio src_id = session_id), this works for arbitrary
// online player ids — used by the DLL to populate group rosters from
// client_state.
//
// Phase A: prefer the outbound bridge link (RPC). Fall back to HTTP
// when no bridge is connected.
func handlePlayerNameQuery(c *Client, playerID uint32) {
	out := map[string]interface{}{
		"type":      "player_name_result",
		"player_id": playerID,
	}
	if b := FirstActiveBridge(); b != nil {
		if name, err := b.RPCName(playerID); err == nil {
			out["name"] = name
			_ = c.Send(out)
			return
		} else {
			log.Printf("control: bridge RPC name(%d) failed (%v) — falling back to HTTP", playerID, err)
		}
	}
	if nameEndpoint == "" {
		out["name"] = ""
		_ = c.Send(out)
		return
	}
	url := fmt.Sprintf("%s?player_id=%d", nameEndpoint, playerID)
	client := http.Client{Timeout: 2 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		out["name"] = ""
		_ = c.Send(out)
		return
	}
	defer resp.Body.Close()
	var body struct {
		PlayerID uint32 `json:"player_id"`
		Name     string `json:"name"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&body)
	out["name"] = body.Name
	_ = c.Send(out)
}

// handleNameQuery resolves the character name for a sid the WS client
// is asking about and sends back name_result. Tries bridge RPC first
// (Phase A), falls back to the HTTP /voice/name endpoint.
func handleNameQuery(conn *websocket.Conn, state *topology.State, srcID uint32) {
	target := state.LookupBySID(srcID)
	type nameResult struct {
		Type   string `json:"type"`
		SrcID  uint32 `json:"src_id"`
		Name   string `json:"name"`
	}
	out := nameResult{Type: "name_result", SrcID: srcID}
	if target == nil || target.PlayerID == 0 {
		_ = conn.WriteJSON(out)
		return
	}
	if b := FirstActiveBridge(); b != nil {
		if name, err := b.RPCName(target.PlayerID); err == nil {
			out.Name = name
			_ = conn.WriteJSON(out)
			return
		}
	}
	if nameEndpoint == "" {
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
//
// Phase A: prefer the outbound bridge link (RPC over WS). Fall back
// to the inbound HTTP callback when no bridge is connected — keeps
// pure-Redis deployments backwards-compatible while we transition.
func whoamiLookup(clientIP string, ports []uint16) (uint32, error) {
	if b := FirstActiveBridge(); b != nil {
		pid, err := b.RPCWhoami(clientIP, ports)
		if err == nil {
			return pid, nil
		}
		// Log and fall through to HTTP.
		log.Printf("control: bridge RPC whoami failed (%v) — falling back to HTTP", err)
	}
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
