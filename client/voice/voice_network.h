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

    // Provide auth payload (set before or after Start; if after,
    // ws thread sends once connected).
    void SetAuthToken(const char* token, uint32_t player_id);

    // Send a proximity audio frame. The client doesn't supply
    // position; the service knows it from L2J events.
    void SendProximityFrame(uint16_t seq,
                            const uint8_t* opus_payload, int opus_len);

    // Send a keepalive (header-only) — call every 15s while
    // session_id is valid and not actively transmitting.
    void SendKeepalive();

    bool IsConnected() const;
    uint32_t SessionID() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
