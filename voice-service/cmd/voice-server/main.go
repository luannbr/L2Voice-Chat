// voice-server — UDP audio router + WebSocket control plane.
//
// MVP scope: PROXIMITY ONLY. Sessions register via WS; service tracks
// session_id -> {position, instance_id, udp_addr}. For each incoming
// UDP audio packet on the proximity channel, the router looks up
// nearby sessions in the same instance and forwards the packet
// unmodified (client-side 3D mixing).
//
// Auth is currently STUB: any WS auth message accepted, session_id
// allocated from a counter. L2J HTTP validation comes in Phase 4.
//
// Wire formats: see docs/protocol.md.

package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/luannbr/l2voice/voice-service/internal/audio"
	"github.com/luannbr/l2voice/voice-service/internal/control"
	"github.com/luannbr/l2voice/voice-service/internal/social"
	"github.com/luannbr/l2voice/voice-service/internal/topology"
	"github.com/luannbr/l2voice/voice-service/internal/world"
)

func main() {
	udpAddr := flag.String("udp", ":17666", "UDP listen address for audio")
	wsAddr := flag.String("ws", ":17667", "TCP listen address for WebSocket control")
	redisAddr := flag.String("redis", "", "Redis broker for L2J event subscription (host:port; empty = disabled)")
	redisChan := flag.String("redis-chan", "l2voice:events", "Redis pub/sub channel")
	l2jWhoami := flag.String("l2j-whoami", "http://127.0.0.1:17668/voice/whoami", "L2J bridge /voice/whoami URL for TCP source-port → player_id resolution (required)")
	l2jName := flag.String("l2j-name", "http://127.0.0.1:17668/voice/name", "L2J bridge /voice/name URL for player_id → character name lookup (optional)")
	l2jGroup := flag.String("l2j-group", "http://127.0.0.1:17668/voice/group", "L2J bridge /voice/group URL for party/clan/ally member lookup (required for channels 1-3)")
	echo := flag.Bool("echo", false, "echo every proximity packet back to the sender (loopback test mode)")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Printf("l2voice voice-service starting (udp=%s ws=%s)", *udpAddr, *wsAddr)

	// Network-session state (per voice WS) and game-world state
	// (per L2 character). The router will read mostly from world;
	// topology stays for session lifecycle (UDP addr, sid alloc).
	state := topology.NewState()
	worldState := world.NewWorldState()
	_ = worldState   // wired into social subscriber below; future
	                 // phases wire it into audio router + WS control.

	// Wire the L2J /voice/whoami endpoint. Required — voice-service can't
	// resolve a WS connection's player_id without it.
	control.SetWhoamiEndpoint(*l2jWhoami)
	control.SetNameEndpoint(*l2jName)
	audio.SetGroupEndpoint(*l2jGroup)
	log.Printf("control: resolving player_id via %s", *l2jWhoami)
	log.Printf("control: resolving character names via %s", *l2jName)
	log.Printf("audio: resolving party/clan/ally groups via %s", *l2jGroup)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	audioCfg := audio.Config{Echo: *echo}
	// UDP audio router runs in its own goroutine; cancelling ctx stops it.
	go func() {
		if err := audio.Serve(ctx, *udpAddr, state, audioCfg); err != nil {
			log.Fatalf("audio.Serve: %v", err)
		}
	}()

	// WebSocket control plane runs in main goroutine; HTTP handler hosts /ws.
	go func() {
		if err := control.Serve(ctx, *wsAddr, state); err != nil {
			log.Fatalf("control.Serve: %v", err)
		}
	}()

	// Redis subscriber (optional — without it, no spatial routing).
	if *redisAddr != "" {
		go func() {
			if err := social.Subscribe(ctx,
				social.Config{Addr: *redisAddr, Channel: *redisChan},
				state, worldState); err != nil {
				log.Printf("social.Subscribe exited: %v", err)
			}
		}()
	} else {
		log.Printf("social: no -redis set; spatial routing disabled (echo mode still works)")
	}

	// Block until SIGINT/SIGTERM.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	sig := <-sigs
	log.Printf("received %v, shutting down...", sig)
	cancel()
}
