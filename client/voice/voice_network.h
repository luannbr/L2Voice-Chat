// voice_network.h — UDP audio socket + WebSocket control channel.
//
// Two long-lived background threads:
//
//   wsThread:  connects to ws_url, sends auth, parses auth_ok to
//              learn UDP endpoint + session_id. Re-connects with
//              exponential backoff on disconnect.
//
//   udpThread: blocks on recvfrom; for each packet, dispatches to
//              the PacketCallback which decodes + enqueues into the
//              playback mixer.
//
// Sending is synchronous from the capture callback (encode then
// sendto) — UDP send is non-blocking and bounded latency.
//
// Wire format note (protocol rev 2): the client does NOT carry
// position. The service computes gain+pan from server-side L2J
// positions and stamps them into the egress packet. This header
// is therefore tiny on send (8 bytes) and 10 bytes on receive for
// the proximity channel.

#pragma once

#include <cstdint>
#include <functional>

namespace voice {

// Callback fired when the service confirms auth and assigns a session.
using AuthOkCallback = std::function<void(uint32_t session_id,
                                          const char* udp_host,
                                          uint16_t udp_port)>;

// Callback fired for each incoming audio packet. The service has
// already computed gain and pan for the proximity channel; non-
// proximity channels arrive with gain=255 / pan=0.
using PacketCallback = std::function<void(uint8_t  channel,
                                          uint32_t src_session_id,
                                          uint16_t seq,
                                          uint8_t  gain,    // 0..255
                                          int8_t   pan,     // -127..+127
                                          const uint8_t* opus_payload,
                                          uint16_t opus_len)>;

class VoiceNetwork {
public:
    VoiceNetwork();
    ~VoiceNetwork();

    // Starts the WS and UDP threads. ws_url like "ws://host:port/path".
    bool Start(const char* ws_url, AuthOkCallback on_auth,
               PacketCallback on_packet);
    void Stop();

    // Provide the list of TCP local ports owned by this process. The
    // bridge correlates these against L2GameClient remote ports to
    // identify which player_id this WS connection represents.
    // Replaces the old token/player_id auth — no client-side identity
    // is trusted; everything is resolved server-side.
    void SetClientPorts(const uint16_t* ports, size_t count);

    // Send a proximity audio frame. The client doesn't supply
    // position; the service knows it from L2J events.
    void SendProximityFrame(uint16_t seq,
                            const uint8_t* opus_payload, int opus_len);

    // Send a group-voice (party/clan/ally) audio frame. channel must
    // be 1 (party), 2 (clan), or 3 (ally). Wire format is the same
    // 8-byte ingress header — voice-service resolves membership
    // server-side via the bridge.
    void SendGroupFrame(uint8_t channel, uint16_t seq,
                        const uint8_t* opus_payload, int opus_len);

    // Send a keepalive (header-only) — call every 15s while
    // session_id is valid and not actively transmitting.
    void SendKeepalive();

    bool IsConnected() const;
    uint32_t SessionID() const;
    // Player id that the voice-service resolved for this session
    // (forwarded back in auth_ok). 0 until auth_ok arrives.
    uint32_t PlayerID() const;

    // Asks the voice-service for the character name behind a given
    // session id. Result arrives asynchronously and is cached;
    // CachedName fills out the buffer with whatever's known. Empty
    // string until the server replies.
    void SendNameQuery(uint32_t src_id);
    bool CachedName(uint32_t src_id, char* out, size_t cap);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
