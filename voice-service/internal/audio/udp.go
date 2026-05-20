// Package audio implements the UDP audio router (protocol rev 2).
//
// Ingress packet shape (8-byte header, no position):
//     version(1) | channel(1) | seq_lo(2) | session_id(4) | opus...
//
// Egress packet shape:
//   - proximity (channel 0):  8-byte header | gain(1) | pan(1) | opus
//   - other channels:         8-byte header | opus
//
// Spatial state (X/Y/Z, InstanceID) is populated by the L2J bridge
// over Redis pub/sub — see internal/social. The router consults the
// topology state for every routed packet and computes gain+pan per
// receiver before sendto.
//
// Packets from un-registered senders (unknown session_id) are
// dropped silently; a 10-second log sample reports the rate.
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
	// Spatial parameters (could be made per-receiver later).
	MinDistance float32 = 500.0  //  5 m in L2 cm — full volume below
	MaxDistance float32 = 2500.0 // 25 m in L2 cm — silent at/above
	// PanHalfWidth: how far off-axis horizontally produces full L/R pan.
	PanHalfWidth float32 = 1500.0

	maxPacketSize = 1500
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

// rxCount is a session-id keyed counter of inbound proximity packets,
// used only for diagnostic log throttling. Not thread-safe by itself,
// but the UDP loop is single-threaded so concurrent writes can't
// happen.
var rxCount = map[uint32]uint64{}

// Config bundles router-mode flags.
type Config struct {
	// Echo: every proximity packet is bounced back to its sender
	// with gain=255 / pan=0 (in addition to normal spatial routing).
	// Useful for loopback validation before there are two clients.
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
			continue
		}
		version := buf[0]
		channel := buf[1]
		seqLo := binary.LittleEndian.Uint16(buf[2:4])
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
		if sess.UDPAddr == nil || sess.UDPAddr.String() != from.String() {
			log.Printf("audio: sid=%d learned UDPAddr=%s (ch=%d)", sid, from, channel)
			state.RememberUDP(sess, from)
		}

		switch channel {
		case chProximity:
			routeProximity(seqLo, sid, buf[8:n], sess, state, conn, cfg)
		case chKeepalive:
			// LastSeen already touched on first packet via RememberUDP path.
		case chPingReq:
			// Reflect as ping_resp with same seq.
			var out [8]byte
			out[0] = 1
			out[1] = chPingResp
			binary.LittleEndian.PutUint16(out[2:4], seqLo)
			binary.LittleEndian.PutUint32(out[4:8], sid)
			conn.WriteToUDP(out[:], from)
		default:
			// party/clan/ally/siege not implemented in MVP — drop quietly.
		}
	}
}

// routeProximity forwards a proximity opus payload to every nearby
// session in the same instance, stamping a per-receiver gain+pan.
func routeProximity(seqLo uint16, srcSID uint32, opus []byte,
	speaker *topology.Session, state *topology.State,
	conn *net.UDPConn, cfg Config) {

	if len(opus) == 0 {
		return
	}

	now := time.Now()
	speakerHasPos := speaker.PositionKnown(now)

	// Diagnostic: per-session rolling counter so the operator can see
	// audio is actually arriving server-side.
	rxCount[srcSID]++
	if rxCount[srcSID]%50 == 1 {
		log.Printf("audio: sid=%d rx=#%d speakerPos=%v",
			srcSID, rxCount[srcSID], speakerHasPos)
	}

	// Egress buffer (header + spatial + opus). Reused per send.
	out := make([]byte, 10+len(opus))
	out[0] = 1                 // version
	out[1] = chProximity       // channel
	binary.LittleEndian.PutUint16(out[2:4], seqLo)
	binary.LittleEndian.PutUint32(out[4:8], srcSID)
	copy(out[10:], opus)

	// Optional self-echo (no spatial info — gain=255, pan=0).
	if cfg.Echo && speaker.UDPAddr != nil {
		out[8] = 255
		out[9] = 0
		_, _ = conn.WriteToUDP(out, speaker.UDPAddr)
	}

	// Without speaker position we can't compute spatial routing.
	// (Service can still deliver in echo mode above for loopback.)
	if !speakerHasPos {
		return
	}

	neighbors := state.ProximityNeighbors(speaker, MaxDistance)
	if rxCount[srcSID]%50 == 1 {
		log.Printf("audio: sid=%d neighbors=%d (max range %.0fcm)",
			srcSID, len(neighbors), MaxDistance)
	}
	for _, recv := range neighbors {
		if recv.UDPAddr == nil {
			if rxCount[srcSID]%50 == 1 {
				log.Printf("audio:   skip recv sid=%d — no UDPAddr yet", recv.ID)
			}
			continue
		}
		if !recv.PositionKnown(now) {
			if rxCount[srcSID]%50 == 1 {
				log.Printf("audio:   skip recv sid=%d — no position", recv.ID)
			}
			continue
		}
		dx := speaker.X - recv.X
		dy := speaker.Y - recv.Y
		dz := speaker.Z - recv.Z
		dist := float32(math.Sqrt(float64(dx*dx + dy*dy + dz*dz)))
		gainF := float32(1.0)
		if dist > MinDistance {
			gainF = 1.0 - (dist-MinDistance)/(MaxDistance-MinDistance)
		}
		if gainF <= 0 {
			continue
		}
		// Pan: horizontal X delta as a stand-in until we have camera yaw.
		panF := dx / PanHalfWidth
		if panF < -1 {
			panF = -1
		} else if panF > 1 {
			panF = 1
		}

		out[8] = uint8(gainF*255 + 0.5)
		out[9] = byte(int8(panF*127 + 0.5))
		n, werr := conn.WriteToUDP(out, recv.UDPAddr)
		if rxCount[srcSID]%50 == 1 {
			log.Printf("audio:   -> recv sid=%d dist=%.0f gain=%d wrote=%d err=%v",
				recv.ID, dist, out[8], n, werr)
		}
	}
}
