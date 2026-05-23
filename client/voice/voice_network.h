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

// Callback fired when the WS transport opens — BEFORE auth happens.
// Lets the caller refresh its TCP-port snapshot so the immediate
// TrySendAuth that follows has fresh data. Without this, the DLL
// would have to wait for the next periodic poll (~1s+) to retry.
using WsOpenCallback = std::function<void()>;

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

    // Optional: register a hook that runs synchronously inside the
    // IXWebSocket Open handler, BEFORE the immediate TrySendAuth.
    // Use it to refresh per-session state (e.g., the L2 TCP port
    // list) so the very first auth attempt on each reconnect has
    // fresh inputs. Must be set before Start.
    void SetWsOpenHook(WsOpenCallback on_open);

    // Soft-suspend the WS without tearing down the whole VoiceNetwork.
    // Closes the current connection and disables auto-reconnect; safe
    // to call multiple times. UDP socket stays open.
    void SuspendWs();
    // Re-enable auto-reconnect and start a new WS connection.
    void ResumeWs();
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

    // Send a WS ping. The voice-service echoes back the same timestamp
    // as a "pong" message, from which we compute the round-trip time.
    // Call periodically from the render loop (~every 2 s is fine).
    void SendPingTick();
    // Latest round-trip time in milliseconds, or -1 if no reply yet.
    int  LastPingMs() const;

    // ---- Notification toasts -------------------------------------------
    // Server-pushed user-facing notifications (remote mute, etc.) are
    // queued here for the overlay to render. Auto-expire after ~5 s.
    struct Toast {
        uint32_t id;             // monotonic, unique per toast
        char     text[192];      // pre-formatted message
        uint8_t  severity;       // 0=info, 1=warn, 2=error, 3=success
        int64_t  created_ms;     // steady_clock ms
        int64_t  dismiss_ms;     // steady_clock ms when it should fade out
    };
    // Copies up to `cap` active (non-expired) toasts into `out`. Returns
    // count written. cap=0/out=nullptr → just query the count.
    size_t GetToasts(Toast* out, size_t cap) const;

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

    // ---- Group roster (from server's client_state push) ---------------
    // The server sends a `client_state` message on auth and on every
    // relevant world change. The DLL caches the snapshot and exposes
    // it to the overlay through these accessors. Thread-safe.

    struct GroupMember {
        uint32_t player_id;
        bool     voice_active;   // has a live voice session right now
        uint8_t  clan_role;      // clan tab only: 0=member,1=sub,2=leader
        bool     cc_can_speak;   // CC tab only
        bool     remote_muted;   // clan/ally only — server-side remote mute is active on this member
    };

    // 0..3 = Party/Clan/Ally/CC. Returns the count written. Pass cap=0
    // to query size only (no copy).
    size_t GetGroupMembers(uint8_t group, GroupMember* out, size_t cap) const;

    // The local player's own role (in their clan). 0=member,1=sub,2=leader.
    uint8_t LocalRole() const;

    // Local player's clan/ally/party/cc identifiers (0 = none).
    uint32_t LocalClanID() const;
    uint32_t LocalAllyID() const;
    uint64_t LocalPartyID() const;
    uint32_t LocalCCID() const;
    uint32_t LocalCCLeaderID() const;
    bool     LocalCCCanSpeak() const;
    uint8_t  LocalClanMode() const;

    // Asks the voice-service for the character name behind a given
    // L2 player id (different from src_id which is a session_id used
    // for audio-source naming). Result cached just like name_query.
    void SendPlayerNameQuery(uint32_t player_id);
    bool CachedPlayerName(uint32_t player_id, char* out, size_t cap);

    // ---- Clan-voice control commands (Phase C of the spec) -------------
    // All of these are no-ops if the WS is not connected. The server is
    // the source of truth — it validates authorization on every command.
    void SendSetChannelEnabled(uint8_t channel, bool enabled);
    void SendSetChannelVolume(uint8_t channel, float volume);
    void SendSetPlayerMute(uint32_t target_player_id, bool muted);
    void SendSetPlayerVolume(uint32_t target_player_id, float volume);
    void SendClanSetMode(uint8_t mode);                    // 0=None, 1=PVP, 2=Siege, 3=Boss, 4=Farm
    void SendWhisperSet(uint8_t mode, int vk);             // 0=off, 1=PTT, 2=always
    void SendClanPromoteSubLeader(uint32_t target_player_id, bool promote);
    void SendClanRemoteMute(uint32_t target_player_id, bool muted, const char* scope);
    void SendClanOptOutUnified(bool opt_out);
    void SendCCGrantSpeak(uint32_t target_player_id, bool granted);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
