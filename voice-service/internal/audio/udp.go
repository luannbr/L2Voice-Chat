// Package audio implements the UDP audio router. One goroutine reads
// packets from the socket, parses the header, and forwards the
// payload to the appropriate set of receivers based on channel:
//
//   channel=0 proximity: spatial lookup in topology, forward to all
//                        sessions within MaxAudibleRange of the sender
//                        in the same instance, ONLY when the sender is
//                        a registered session (via WS auth).
//   channel=1..3 party/clan/ally: deferred (not implemented in MVP).
//   channel=5 keepalive: noop, just refreshes LastSeen.
//   channel=6 ping_req: echo back as channel=7 to the same sender.
//
// Packets from un-registered senders (unknown session_id) are
// dropped silently; logged on a 10-second sampling cadence to avoid
// log floods.
package audio

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"math"
	"net"
	"time"

	"github.com/luannbr/l2voice/voice-service/internal/topology"
)

const (
	// MaxAudibleRange is the cutoff for proximity routing. Beyond
	// this distance the speaker is inaudible regardless of attenuation
	// curve. Stays comfortably above the 25m fade-to-zero in the
	// protocol so the client always has a packet to ramp out with.
	MaxAudibleRange float32 = 3000.0 // 30 m in L2 cm units

	maxPacketSize = 1500 // safely below typical Ethernet MTU
)

const (
	chProximity uint8 = 0
	chParty     uint8 = 1
	chClan      uint8 = 2
	chAlly      uint8 = 3
	chSiege     uint8 = 4
	chKeepalive uint8 = 5
	chPingReq   uint8 = 6
	chPingResp  uint8 = 7
)

// Config bundles router-mode flags.
type Config struct {
	// Echo: every proximity packet is bounced back to its sender
	// (in addition to normal spatial routing — used for loopback
	// validation of audio capture/codec/UDP path before there are
	// two clients to test with).
	Echo bool
}

// Serve binds udpAddr and routes audio packets until ctx is cancelled.
func Serve(ctx context.Context, udpAddr string, state *topology.State, cfg Config) error {
	addr, err := net.ResolveUDPAddr("udp", udpAddr)
	if err != nil {
		return err
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	log.Printf("audio: listening on %s", conn.LocalAddr())

	// Close on shutdown so the blocking ReadFromUDP returns.
	go func() {
		<-ctx.Done()
		conn.SetReadDeadline(time.Unix(1, 0))
		conn.Close()
	}()

	buf := make([]byte, maxPacketSize)
	var droppedUnknown int
	var lastDropLog time.Time

	for {
		n, from, err := conn.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) || ctx.Err() != nil {
				return nil
			}
			log.Printf("audio: read error: %v", err)
			continue
		}
		if n < 8 {
			continue // smaller than common header
		}
		// Parse common header.
		version := buf[0]
		channel := buf[1]
		// seqLo := binary.LittleEndian.Uint16(buf[2:4])
		sid := binary.LittleEndian.Uint32(buf[4:8])
		if version != 1 {
			continue
		}

		sess := state.LookupBySID(sid)
		if sess == nil {
			droppedUnknown++
			if time.Since(lastDropLog) > 10*time.Second {
				log.Printf("audio: dropped %d packets from unknown sessions",
					droppedUnknown)
				droppedUnknown = 0
				lastDropLog = time.Now()
			}
			continue
		}
		// First packet (or rebind): record source addr.
		if sess.UDPAddr == nil || sess.UDPAddr.String() != from.String() {
			state.RememberUDP(sess, from)
		}

		switch channel {
		case chProximity:
			routeProximity(buf[:n], sess, state, conn)
			if cfg.Echo {
				// Echo back to sender for loopback testing.
				_, _ = conn.WriteToUDP(buf[:n], from)
			}
		case chKeepalive:
			// noop, RememberUDP already touched LastSeen via RememberUDP path
		case chPingReq:
			// echo back as resp
			out := make([]byte, n)
			copy(out, buf[:n])
			out[1] = chPingResp
			conn.WriteToUDP(out, from)
		default:
			// party/clan/ally/siege not implemented in MVP — drop quietly
		}
	}
}

// routeProximity parses the position from a channel-0 packet, updates
// the speaker's position in topology, then forwards the original
// packet bytes to every nearby session in the same instance.
func routeProximity(pkt []byte, speaker *topology.Session,
	state *topology.State, conn *net.UDPConn) {

	if len(pkt) < 28 {
		return // truncated proximity packet
	}
	x := math.Float32frombits(binary.LittleEndian.Uint32(pkt[8:12]))
	y := math.Float32frombits(binary.LittleEndian.Uint32(pkt[12:16]))
	z := math.Float32frombits(binary.LittleEndian.Uint32(pkt[16:20]))
	inst := binary.LittleEndian.Uint32(pkt[20:24])
	state.UpdatePosition(speaker, x, y, z, inst)

	for _, recv := range state.ProximityNeighbors(speaker, MaxAudibleRange) {
		if recv.UDPAddr == nil {
			continue
		}
		_, _ = conn.WriteToUDP(pkt, recv.UDPAddr)
	}
}

