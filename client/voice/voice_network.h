// voice_network.h — UDP audio socket + WebSocket control channel.
//
// Two long-lived background threads:
//
//   wsThread:  connects to ws_url, sends auth, parses auth_ok to
//              learn UDP endpoint + session_id. Re-connects with
//              exponential backoff on disconnect.
//
//   udpThread: blocks on recvfrom; for each packet, dispatches to
//              VoicePipeline::OnRecv which decodes + enqueues into
//              the playback mixer.
//
// Sending is synchronous from the capture callback (encode then
// sendto) — UDP send is non-blocking and bounded latency.

#pragma once

#include <cstdint>
#include <functional>

namespace voice {

// Callback fired when the service confirms auth and assigns a session.
using AuthOkCallback = std::function<void(uint32_t session_id,
                                          const char* udp_host,
                                          uint16_t udp_port)>;

// Callback fired for each incoming audio packet. Buffer ownership is
// caller's; the callback should copy or finish work before returning.
//
// channel matches the protocol enum. For proximity, x/y/z/instance
// are the sender's position. For other channels they're zero.
using PacketCallback = std::function<void(uint8_t channel,
                                          uint32_t src_session_id,
                                          uint32_t seq,
                                          float x, float y, float z,
                                          uint32_t instance_id,
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

    // Send a proximity packet from the local mic.
    // sess_id is the session_id from auth_ok (cached in the network).
    // opus_payload may be any size up to kMaxPacketBytes.
    void SendProximityFrame(uint32_t seq,
                            float x, float y, float z, uint32_t instance_id,
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
