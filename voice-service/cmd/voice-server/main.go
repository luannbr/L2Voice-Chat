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
	"github.com/luannbr/l2voice/voice-service/internal/topology"
)

func main() {
	udpAddr := flag.String("udp", ":17666", "UDP listen address for audio")
	wsAddr := flag.String("ws", ":17667", "TCP listen address for WebSocket control")
	echo := flag.Bool("echo", false, "echo every proximity packet back to the sender (loopback test mode)")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Printf("l2voice voice-service starting (udp=%s ws=%s)", *udpAddr, *wsAddr)

	// Shared state across UDP and WS handlers.
	state := topology.NewState()

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

	// Block until SIGINT/SIGTERM.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	sig := <-sigs
	log.Printf("received %v, shutting down...", sig)
	cancel()
}
