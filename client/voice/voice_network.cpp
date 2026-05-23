// voice_network.cpp — UDP audio socket + IXWebSocket control plane.
//
// WS thread: connects to ws_url, sends an "auth" JSON envelope when
// it has a token, parses "auth_ok" to learn (session_id, udp_host,
// udp_port). Re-connects with exponential backoff (built into
// IXWebSocket).
//
// UDP thread: blocks on recvfrom; for each packet, parses the
// 8-byte common header and (channel == proximity ? the 2-byte
// gain+pan suffix) and fires PacketCallback.
//
// Send is fire-and-forget. RTT measurement via ping_req/ping_resp
// can be wired later via a small map of seq → t_send.

#include "voice_network.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

namespace voice {

namespace {

std::string MakeAuthJson(const std::vector<uint16_t>& ports) {
    std::string s = "{\"type\":\"auth\",\"ports\":[";
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(ports[i]);
    }
    s += "]}";
    return s;
}

bool ExtractString(const std::string& s, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    auto p = s.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    auto q = s.find('"', p);
    if (q == std::string::npos) return false;
    out.assign(s, p, q - p);
    return true;
}

bool ExtractNumber(const std::string& s, const char* key, uint64_t& out) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = s.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    uint64_t v = 0; bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        v = v * 10 + (s[p] - '0');
        ++p; any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Extracts a boolean field. Returns false when the key is absent or
// the literal value is neither true nor false.
bool ExtractBool(const std::string& s, const char* key, bool& out) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = s.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    if (s.compare(p, 4, "true") == 0)  { out = true;  return true; }
    if (s.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

// Slices the body of an array field. On success returns true and sets
// [body_start, body_end) to the inclusive-exclusive bounds of the
// content between the brackets. Brace-depth-aware so nested {} inside
// objects don't confuse it.
bool ExtractArrayBody(const std::string& s, const char* key,
                      size_t& body_start, size_t& body_end) {
    std::string needle = std::string("\"") + key + "\":[";
    auto p = s.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    body_start = p;
    int depth = 0;
    while (p < s.size()) {
        char c = s[p];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        else if (c == ']' && depth == 0) {
            body_end = p;
            return true;
        }
        ++p;
    }
    return false;
}

}  // namespace

struct VoiceNetwork::Impl {
    std::string ws_url;
    std::mutex auth_mu;
    std::vector<uint16_t> client_ports;

    std::atomic<uint32_t> session_id{0};
    std::atomic<uint32_t> player_id_resolved{0};
    std::string udp_host;
    std::atomic<uint16_t> udp_port{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> auth_sent{false};

    AuthOkCallback on_auth;
    PacketCallback on_packet;
    WsOpenCallback on_ws_open;   // fires inside the IXWebSocket Open handler

    SOCKET udp_sock = INVALID_SOCKET;
    sockaddr_in udp_dest{};
    ix::WebSocket ws;
    std::thread udp_thread;
    std::atomic<bool> stopping{false};

    // sid → character name cache. Populated by name_result messages
    // from the voice-service. Read by the overlay every frame.
    std::mutex names_mu;
    std::unordered_map<uint32_t, std::string> name_cache;
    std::unordered_map<uint32_t, bool> name_query_inflight;

    // player_id → name (separate cache, populated by player_name_result).
    std::unordered_map<uint32_t, std::string> player_name_cache;
    std::unordered_map<uint32_t, bool>        player_name_inflight;

    // ---- Notification toasts (server-pushed) ---------------------
    mutable std::mutex            toasts_mu;
    std::vector<VoiceNetwork::Toast> toasts;
    std::atomic<uint32_t>         next_toast_id{1};
    void AddToast(const char* text, uint8_t severity, int64_t ttl_ms);

    // ---- WS ping/pong (RTT measurement) --------------------------
    // Uses steady_clock so wall-clock changes don't pollute. The
    // last-sent timestamp doubles as the in-flight token; the server
    // echoes it back in "pong" and we subtract to get RTT.
    std::atomic<int64_t> last_ping_sent_ms{0};
    std::atomic<int>     last_rtt_ms{-1};

    // ---- client_state snapshot (Phase D server push) -------------
    // Mutex covers the entire blob since the overlay reads via copy.
    std::mutex                                 state_mu;
    uint8_t   cs_role{0};
    uint32_t  cs_clan_id{0};
    uint32_t  cs_ally_id{0};
    uint64_t  cs_party_id{0};
    uint32_t  cs_cc_id{0};
    uint32_t  cs_cc_leader_id{0};
    bool      cs_cc_can_speak{false};
    uint8_t   cs_clan_mode{0};
    bool      cs_opted_out{false};
    std::vector<VoiceNetwork::GroupMember> cs_party_members;
    std::vector<VoiceNetwork::GroupMember> cs_clan_members;
    std::vector<VoiceNetwork::GroupMember> cs_ally_members;
    std::vector<VoiceNetwork::GroupMember> cs_cc_members;
    void HandleClientStateImpl(const std::string& s);
    void HandleNotification(const std::string& s);

    bool InitWinsock() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    bool OpenUdp() {
        udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_sock == INVALID_SOCKET) return false;
        DWORD tv = 100;
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&tv, sizeof(tv));
        return true;
    }

    void SetUdpDest(const std::string& host, uint16_t port) {
        std::memset(&udp_dest, 0, sizeof(udp_dest));
        udp_dest.sin_family = AF_INET;
        udp_dest.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &udp_dest.sin_addr);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = 0;
        bind(udp_sock, (sockaddr*)&local, sizeof(local));
    }

    void UdpLoop() {
        uint8_t buf[2048];
        sockaddr_in from{};
        int from_len = sizeof(from);
        while (!stopping.load()) {
            int n = recvfrom(udp_sock, (char*)buf, sizeof(buf), 0,
                             (sockaddr*)&from, &from_len);
            if (n < 8) continue;
            uint8_t ver = buf[0];
            if (ver != 1) continue;
            uint8_t channel = buf[1];
            uint16_t seq = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
            uint32_t src = (uint32_t)buf[4]
                         | ((uint32_t)buf[5] << 8)
                         | ((uint32_t)buf[6] << 16)
                         | ((uint32_t)buf[7] << 24);

            const uint8_t* opus = nullptr;
            uint16_t opus_len = 0;
            uint8_t gain = 255;
            int8_t  pan  = 0;

            // All audio channels use a uniform 10-byte header:
            //     v(1) | ch(1) | seq(2) | sid(4) | gain(1) | pan(1) | opus...
            // Egress channel mapping:
            //   0 proximity, 1 party, 2 clan, 3 ally, 4 CC, 8 mode-unified.
            // (5/6/7 are control frames — never carry audio on egress.)
            if (channel == 0 ||
                (channel >= 1 && channel <= 4) ||
                channel == 8) {
                if (n < 10) continue;
                gain = buf[8];
                pan  = (int8_t)buf[9];
                opus = buf + 10;
                opus_len = (uint16_t)(n - 10);
            } else if (channel == 7) {          // ping_resp
                continue;
            } else {
                continue;
            }

            if (on_packet) {
                on_packet(channel, src, seq, gain, pan, opus, opus_len);
            }
        }
    }

    void StartWs() {
        ws.setUrl(ws_url);
        ws.setPingInterval(20);
        // Cap reconnect backoff so "first audio after EnterWorld" is
        // sub-second. Default max is 10 s+ which gets ugly if the
        // server has been closing pre-auth attempts during login.
        ws.setMinWaitBetweenReconnectionRetries(200);   // ms
        ws.setMaxWaitBetweenReconnectionRetries(2000);  // ms
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    connected.store(true);
                    auth_sent.store(false);
                    // Snapshot fresh per-session state (e.g. TCP port
                    // list) before the immediate TrySendAuth so the
                    // FIRST attempt on each reconnect can succeed.
                    if (on_ws_open) on_ws_open();
                    TrySendAuth();
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    connected.store(false);
                    auth_sent.store(false);
                    // Clear the auth state so the overlay hides until
                    // the next auth_ok arrives. Without this, going
                    // from in-world back to char-select would leave
                    // the panel visible during login.
                    session_id.store(0);
                    player_id_resolved.store(0);
                    udp_port.store(0);
                    break;
                case ix::WebSocketMessageType::Message:
                    HandleWsMessage(msg->str);
                    break;
                default:
                    break;
            }
        });
        ws.start();
    }

    void TrySendAuth() {
        std::lock_guard<std::mutex> lk(auth_mu);
        if (client_ports.empty()) {
            OutputDebugStringA("[l2voice] TrySendAuth: no client_ports yet\n");
            return;
        }
        if (auth_sent.load()) return;
        if (!connected.load()) {
            OutputDebugStringA("[l2voice] TrySendAuth: not connected yet\n");
            return;
        }
        char dbg[256];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] TrySendAuth: sending %zu candidate ports\n",
            client_ports.size());
        OutputDebugStringA(dbg);
        ws.send(MakeAuthJson(client_ports));
        auth_sent.store(true);
    }

    void HandleWsMessage(const std::string& s) {
        // name_result {type, src_id, name}
        if (s.find("\"name_result\"") != std::string::npos) {
            uint64_t sid_u = 0;
            std::string nm;
            ExtractNumber(s, "src_id", sid_u);
            ExtractString(s, "name", nm);
            {
                std::lock_guard<std::mutex> lk(names_mu);
                name_query_inflight.erase((uint32_t)sid_u);
                if (!nm.empty()) {
                    name_cache[(uint32_t)sid_u] = nm;
                }
            }
            return;
        }
        // player_name_result {type, player_id, name}
        if (s.find("\"player_name_result\"") != std::string::npos) {
            uint64_t pid = 0;
            std::string nm;
            ExtractNumber(s, "player_id", pid);
            ExtractString(s, "name", nm);
            {
                std::lock_guard<std::mutex> lk(names_mu);
                player_name_inflight.erase((uint32_t)pid);
                if (!nm.empty()) player_name_cache[(uint32_t)pid] = nm;
            }
            return;
        }
        if (s.find("\"client_state\"") != std::string::npos) {
            HandleClientStateImpl(s);
            return;
        }
        // notification {type, subtype, from, scope, muted, ...}
        // Currently the server emits subtype="remotely_muted_by" when
        // a clan leader/sub mutes (or unmutes) a member. The DLL queues
        // a toast with the human-readable text; the overlay renders.
        if (s.find("\"notification\"") != std::string::npos) {
            HandleNotification(s);
            return;
        }
        // pong {type, t}: t is whatever we sent in the corresponding
        // ping (a steady_clock ms). Diff against "now" = round-trip.
        if (s.find("\"pong\"") != std::string::npos) {
            uint64_t t_echo = 0;
            if (ExtractNumber(s, "t", t_echo)) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                int rtt = (int)(now_ms - (int64_t)t_echo);
                if (rtt < 0) rtt = 0;
                if (rtt > 99999) rtt = 99999;
                last_rtt_ms.store(rtt);
            }
            return;
        }
        if (s.find("\"auth_ok\"") == std::string::npos) {
            // Other message types (auth_fail, topology_update, etc.) —
            // not handled in this minimal client.
            return;
        }
        uint64_t sid_u = 0;
        if (!ExtractNumber(s, "session_id", sid_u)) {
            OutputDebugStringA("[l2voice] auth_ok missing session_id\n");
            return;
        }
        // Voice-service sends a single "udp_endpoint":"host:port" string.
        std::string endpoint;
        if (!ExtractString(s, "udp_endpoint", endpoint)) {
            OutputDebugStringA("[l2voice] auth_ok missing udp_endpoint\n");
            return;
        }
        auto colon = endpoint.rfind(':');
        if (colon == std::string::npos) {
            OutputDebugStringA("[l2voice] udp_endpoint missing ':'\n");
            return;
        }
        std::string host = endpoint.substr(0, colon);
        uint16_t port = (uint16_t)std::stoi(endpoint.substr(colon + 1));
        if (port == 0) {
            OutputDebugStringA("[l2voice] udp_endpoint port=0\n");
            return;
        }
        session_id.store((uint32_t)sid_u);
        uint64_t pid_u = 0;
        if (ExtractNumber(s, "your_player_id", pid_u)) {
            player_id_resolved.store((uint32_t)pid_u);
        }
        udp_host = host;
        udp_port.store(port);
        SetUdpDest(host, port);
        char dbg[200];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] auth_ok session_id=%u player_id=%u udp_endpoint=%s:%u\n",
            (uint32_t)sid_u, (uint32_t)pid_u, host.c_str(), port);
        OutputDebugStringA(dbg);
        if (on_auth) on_auth((uint32_t)sid_u, host.c_str(), port);
    }
};

VoiceNetwork::VoiceNetwork()  : impl_(new Impl()) {}
VoiceNetwork::~VoiceNetwork() { Stop(); delete impl_; }

bool VoiceNetwork::Start(const char* ws_url, AuthOkCallback on_auth,
                         PacketCallback on_packet) {
    if (!impl_->InitWinsock())   return false;
    ix::initNetSystem();
    if (!impl_->OpenUdp())       return false;
    impl_->ws_url    = ws_url;
    impl_->on_auth   = std::move(on_auth);
    impl_->on_packet = std::move(on_packet);
    impl_->stopping.store(false);
    impl_->udp_thread = std::thread([this] { impl_->UdpLoop(); });
    impl_->StartWs();
    return true;
}

void VoiceNetwork::SetWsOpenHook(WsOpenCallback on_open) {
    impl_->on_ws_open = std::move(on_open);
}

void VoiceNetwork::SuspendWs() {
    if (!impl_) return;
    // Disable auto-reconnect first; otherwise stop() triggers the
    // close handler which would just bounce back via setOnMessage's
    // automatic-reconnect path.
    impl_->ws.disableAutomaticReconnection();
    impl_->ws.stop();
    impl_->connected.store(false);
    impl_->auth_sent.store(false);
    impl_->session_id.store(0);
    impl_->udp_port.store(0);
}

void VoiceNetwork::ResumeWs() {
    if (!impl_) return;
    impl_->ws.enableAutomaticReconnection();
    impl_->ws.start();
}

void VoiceNetwork::Stop() {
    if (!impl_) return;
    impl_->stopping.store(true);
    if (impl_->udp_sock != INVALID_SOCKET) {
        closesocket(impl_->udp_sock);
        impl_->udp_sock = INVALID_SOCKET;
    }
    if (impl_->udp_thread.joinable()) impl_->udp_thread.join();
    impl_->ws.stop();
    ix::uninitNetSystem();
    WSACleanup();
}

void VoiceNetwork::SetClientPorts(const uint16_t* ports, size_t count) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(impl_->auth_mu);
        if (impl_->client_ports.size() != count) {
            changed = true;
        } else {
            for (size_t i = 0; i < count; ++i) {
                if (impl_->client_ports[i] != ports[i]) {
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            impl_->client_ports.assign(ports, ports + count);
            impl_->auth_sent.store(false);
        }
    }
    if (changed) impl_->TrySendAuth();
}

void VoiceNetwork::SendProximityFrame(uint16_t seq,
                                      const uint8_t* opus, int opus_len) {
    if (impl_->udp_port.load() == 0) {
        static int once = 0;
        if (once++ == 0) {
            OutputDebugStringA("[l2voice] SendProximityFrame skipped — udp_port==0 (auth_ok not parsed)\n");
        }
        return;
    }
    if (opus_len <= 0 || opus_len > 1024) return;

    uint8_t pkt[1100];
    pkt[0] = 1;                       // version
    pkt[1] = 0;                       // channel = proximity
    pkt[2] = (uint8_t)(seq & 0xFF);
    pkt[3] = (uint8_t)((seq >> 8) & 0xFF);
    uint32_t sid = impl_->session_id.load();
    std::memcpy(pkt + 4, &sid, 4);
    std::memcpy(pkt + 8, opus, opus_len);
    int total = 8 + opus_len;
    sendto(impl_->udp_sock, (const char*)pkt, total, 0,
           (sockaddr*)&impl_->udp_dest, sizeof(impl_->udp_dest));
}

void VoiceNetwork::SendGroupFrame(uint8_t channel, uint16_t seq,
                                  const uint8_t* opus, int opus_len) {
    if (impl_->udp_port.load() == 0) return;
    if (opus_len <= 0 || opus_len > 1024) return;
    if (channel < 1 || channel > 3) return;
    uint8_t pkt[1100];
    pkt[0] = 1;                       // version
    pkt[1] = channel;                 // 1=party, 2=clan, 3=ally
    pkt[2] = (uint8_t)(seq & 0xFF);
    pkt[3] = (uint8_t)((seq >> 8) & 0xFF);
    uint32_t sid = impl_->session_id.load();
    std::memcpy(pkt + 4, &sid, 4);
    std::memcpy(pkt + 8, opus, opus_len);
    int total = 8 + opus_len;
    sendto(impl_->udp_sock, (const char*)pkt, total, 0,
           (sockaddr*)&impl_->udp_dest, sizeof(impl_->udp_dest));
}

void VoiceNetwork::SendPingTick() {
    if (!impl_->connected.load()) return;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Throttle to ~once per 2 s — caller can hammer this from the
    // render loop without worry.
    int64_t last = impl_->last_ping_sent_ms.load();
    if (now_ms - last < 2000) return;
    impl_->last_ping_sent_ms.store(now_ms);
    std::string s = "{\"type\":\"ping\",\"t\":";
    s += std::to_string(now_ms);
    s += "}";
    impl_->ws.send(s);
}

int VoiceNetwork::LastPingMs() const {
    return impl_->last_rtt_ms.load();
}

// Builds a Toast and pushes it into the queue. Older toasts past their
// TTL are pruned opportunistically here so the buffer stays bounded.
void VoiceNetwork::Impl::AddToast(const char* text, uint8_t severity, int64_t ttl_ms) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    VoiceNetwork::Toast t{};
    t.id = next_toast_id.fetch_add(1);
    _snprintf_s(t.text, _TRUNCATE, "%s", text);
    t.severity   = severity;
    t.created_ms = now_ms;
    t.dismiss_ms = now_ms + ttl_ms;
    std::lock_guard<std::mutex> lk(toasts_mu);
    // Prune expired entries first.
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
        [now_ms](const VoiceNetwork::Toast& x){ return x.dismiss_ms <= now_ms; }),
        toasts.end());
    // Cap to last 5 — voice notifications shouldn't ever flood.
    while (toasts.size() >= 5) toasts.erase(toasts.begin());
    toasts.push_back(t);
}

// Handles a notification frame. Currently only "remotely_muted_by".
// Format: {"type":"notification","subtype":"remotely_muted_by",
//          "from":<pid>,"scope":"clan"|"ally","muted":bool}
void VoiceNetwork::Impl::HandleNotification(const std::string& s) {
    // subtype
    std::string subtype;
    ExtractString(s, "subtype", subtype);
    if (subtype == "remotely_muted_by") {
        uint64_t from = 0;
        ExtractNumber(s, "from", from);
        std::string scope;
        ExtractString(s, "scope", scope);
        bool muted = true;
        ExtractBool(s, "muted", muted);

        // Resolve the muter's name via the existing player-name cache
        // (async — first call kicks off a query; on next notification
        // we have the name). For the immediate toast, fall back to
        // "pid=N" if unresolved.
        char muterName[64] = "?";
        {
            std::lock_guard<std::mutex> lk(names_mu);
            auto it = player_name_cache.find((uint32_t)from);
            if (it != player_name_cache.end() && !it->second.empty()) {
                _snprintf_s(muterName, _TRUNCATE, "%s", it->second.c_str());
            } else {
                _snprintf_s(muterName, _TRUNCATE, "pid=%llu", (unsigned long long)from);
            }
        }
        // Kick off a name query so the SECOND notification can use the
        // resolved name (this one may show pid=N if first-sight).
        if (player_name_cache.find((uint32_t)from) == player_name_cache.end()
            && player_name_inflight.find((uint32_t)from) == player_name_inflight.end()) {
            player_name_inflight[(uint32_t)from] = true;
            std::string q = "{\"type\":\"player_name_query\",\"player_id\":";
            q += std::to_string(from);
            q += "}";
            ws.send(q);
        }

        char buf[192];
        if (muted) {
            _snprintf_s(buf, _TRUNCATE,
                "You were muted on %s by %s",
                scope.empty() ? "clan" : scope.c_str(), muterName);
        } else {
            _snprintf_s(buf, _TRUNCATE,
                "%s unmuted you on %s",
                muterName, scope.empty() ? "clan" : scope.c_str());
        }
        // Mute = error/warn, unmute = success.
        AddToast(buf, muted ? 2 : 3, 6000 /*ms*/);
    } else {
        // Forward-compat: unknown subtype gets a generic info toast.
        AddToast(("notification: " + subtype).c_str(), 0, 4000);
    }
}

size_t VoiceNetwork::GetToasts(Toast* out, size_t cap) const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(impl_->toasts_mu);
    // Don't prune from a const accessor — just skip the expired ones
    // here. The producer (AddToast) prunes opportunistically.
    size_t written = 0;
    for (const Toast& t : impl_->toasts) {
        if (t.dismiss_ms <= now_ms) continue;
        if (cap == 0 || out == nullptr) {
            ++written;
            continue;
        }
        if (written >= cap) break;
        out[written++] = t;
    }
    return written;
}

void VoiceNetwork::SendKeepalive() {
    if (impl_->udp_port.load() == 0) return;
    uint8_t pkt[8];
    pkt[0] = 1; pkt[1] = 5; pkt[2] = 0; pkt[3] = 0;
    uint32_t sid = impl_->session_id.load();
    std::memcpy(pkt + 4, &sid, 4);
    sendto(impl_->udp_sock, (const char*)pkt, 8, 0,
           (sockaddr*)&impl_->udp_dest, sizeof(impl_->udp_dest));
}

bool VoiceNetwork::IsConnected() const { return impl_->connected.load(); }
uint32_t VoiceNetwork::SessionID() const { return impl_->session_id.load(); }
uint32_t VoiceNetwork::PlayerID()  const { return impl_->player_id_resolved.load(); }

void VoiceNetwork::SendNameQuery(uint32_t src_id) {
    if (!impl_->connected.load() || src_id == 0) return;
    {
        std::lock_guard<std::mutex> lk(impl_->names_mu);
        if (impl_->name_cache.count(src_id)) return;          // already known
        if (impl_->name_query_inflight.count(src_id)) return; // already asked
        impl_->name_query_inflight[src_id] = true;
    }
    std::string s = "{\"type\":\"name_query\",\"src_id\":";
    s += std::to_string(src_id);
    s += "}";
    impl_->ws.send(s);
}

// ----- Phase C: clan-voice control commands ------------------------
//
// All senders build the JSON inline to avoid pulling a JSON dep into
// the DLL just for tiny one-shot messages. ws.send is thread-safe per
// IXWebSocket's contract.

namespace {
std::string ftos(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}
}

void VoiceNetwork::SendSetChannelEnabled(uint8_t channel, bool enabled) {
    if (!impl_->connected.load()) return;
    std::string s = "{\"type\":\"set_channel_enabled\",\"channel\":";
    s += std::to_string((int)channel);
    s += ",\"enabled\":";
    s += enabled ? "true" : "false";
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendSetChannelVolume(uint8_t channel, float volume) {
    if (!impl_->connected.load()) return;
    std::string s = "{\"type\":\"set_channel_volume\",\"channel\":";
    s += std::to_string((int)channel);
    s += ",\"volume\":";
    s += ftos(volume);
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendSetPlayerMute(uint32_t target_player_id, bool muted) {
    if (!impl_->connected.load() || target_player_id == 0) return;
    std::string s = "{\"type\":\"set_player_mute\",\"target_id\":";
    s += std::to_string(target_player_id);
    s += ",\"muted\":";
    s += muted ? "true" : "false";
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendSetPlayerVolume(uint32_t target_player_id, float volume) {
    if (!impl_->connected.load() || target_player_id == 0) return;
    std::string s = "{\"type\":\"set_player_volume\",\"target_id\":";
    s += std::to_string(target_player_id);
    s += ",\"volume\":";
    s += ftos(volume);
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendClanSetMode(uint8_t mode) {
    if (!impl_->connected.load()) return;
    std::string s = "{\"type\":\"clan_set_mode\",\"mode\":";
    s += std::to_string((int)mode);
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendWhisperSet(uint8_t mode, int vk) {
    if (!impl_->connected.load()) return;
    std::string s = "{\"type\":\"whisper_set\",\"mode\":";
    s += std::to_string((int)mode);
    s += ",\"key\":";
    s += std::to_string(vk);
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendClanPromoteSubLeader(uint32_t target_player_id, bool promote) {
    if (!impl_->connected.load() || target_player_id == 0) return;
    std::string s = "{\"type\":\"";
    s += promote ? "clan_promote_subleader" : "clan_demote_subleader";
    s += "\",\"target_id\":";
    s += std::to_string(target_player_id);
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendClanRemoteMute(uint32_t target_player_id, bool muted, const char* scope) {
    if (!impl_->connected.load() || target_player_id == 0 || !scope) return;
    std::string s = "{\"type\":\"clan_remote_mute\",\"target_id\":";
    s += std::to_string(target_player_id);
    s += ",\"muted\":";
    s += muted ? "true" : "false";
    s += ",\"scope\":\"";
    s += scope;
    s += "\"}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendCCGrantSpeak(uint32_t target_player_id, bool granted) {
    if (!impl_->connected.load() || target_player_id == 0) return;
    std::string s = "{\"type\":\"cc_grant_speak\",\"target_id\":";
    s += std::to_string(target_player_id);
    s += ",\"granted\":";
    s += granted ? "true" : "false";
    s += "}";
    impl_->ws.send(s);
}

void VoiceNetwork::SendClanOptOutUnified(bool opt_out) {
    if (!impl_->connected.load()) return;
    std::string s = "{\"type\":\"clan_opt_out_unified\",\"opt_out\":";
    s += opt_out ? "true" : "false";
    s += "}";
    impl_->ws.send(s);
}

// ----- client_state parsing + accessors ----------------------------

namespace {
// Parses a single member object body (between { and }) starting at
// `body[pos..]`. Returns the offset past the closing '}'. Fills out
// the GroupMember from id/voice_active/role/can_speak keys; absent
// keys default to zero/false.
size_t ParseMemberObject(const std::string& body, size_t pos,
                         VoiceNetwork::GroupMember& m) {
    // Locate the enclosing braces.
    while (pos < body.size() && body[pos] != '{') ++pos;
    if (pos >= body.size()) return std::string::npos;
    size_t start = pos;
    int depth = 0;
    size_t end = pos;
    for (; end < body.size(); ++end) {
        if (body[end] == '{') ++depth;
        else if (body[end] == '}') {
            --depth;
            if (depth == 0) { ++end; break; }
        }
    }
    if (depth != 0) return std::string::npos;
    std::string obj(body, start, end - start);
    m = {};
    uint64_t v = 0;
    if (ExtractNumber(obj, "id", v))    m.player_id = (uint32_t)v;
    bool b = false;
    if (ExtractBool(obj, "voice_active", b)) m.voice_active = b;
    if (ExtractNumber(obj, "role", v)) m.clan_role = (uint8_t)v;
    if (ExtractBool(obj, "can_speak", b)) m.cc_can_speak = b;
    if (ExtractBool(obj, "remote_muted", b)) m.remote_muted = b;
    return end;
}

void ParseMemberArray(const std::string& s, const char* key,
                      std::vector<VoiceNetwork::GroupMember>& out) {
    out.clear();
    size_t bs = 0, be = 0;
    if (!ExtractArrayBody(s, key, bs, be)) return;
    std::string body(s, bs, be - bs);
    size_t pos = 0;
    while (pos < body.size()) {
        VoiceNetwork::GroupMember m{};
        size_t next = ParseMemberObject(body, pos, m);
        if (next == std::string::npos) break;
        if (m.player_id != 0) out.push_back(m);
        pos = next;
    }
}
} // namespace

void VoiceNetwork::Impl::HandleClientStateImpl(const std::string& s) {
    uint64_t v = 0;
    uint8_t role = 0; uint32_t clan = 0, ally = 0, cc = 0, ccLeader = 0;
    uint64_t party = 0;
    bool ccCanSpeak = false; uint8_t mode = 0; bool optedOut = false;
    if (ExtractNumber(s, "role",         v)) role     = (uint8_t)v;
    if (ExtractNumber(s, "clan_id",      v)) clan     = (uint32_t)v;
    if (ExtractNumber(s, "ally_id",      v)) ally     = (uint32_t)v;
    if (ExtractNumber(s, "party_id",     v)) party    = v;
    if (ExtractNumber(s, "cc_id",        v)) cc       = (uint32_t)v;
    if (ExtractNumber(s, "cc_leader_id", v)) ccLeader = (uint32_t)v;
    ExtractBool(s, "cc_can_speak", ccCanSpeak);
    if (ExtractNumber(s, "clan_mode",    v)) mode     = (uint8_t)v;
    ExtractBool(s, "opted_out", optedOut);

    std::vector<GroupMember> party_m, clan_m, ally_m, cc_m;
    ParseMemberArray(s, "party_members", party_m);
    ParseMemberArray(s, "clan_members",  clan_m);
    ParseMemberArray(s, "ally_members",  ally_m);
    ParseMemberArray(s, "cc_members",    cc_m);

    {
        char dbg[256];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] client_state: role=%u clan=%u ally=%u party=%llu cc=%u "
            "members(party/clan/ally/cc)=%zu/%zu/%zu/%zu\n",
            (unsigned)role, clan, ally,
            (unsigned long long)party, cc,
            party_m.size(), clan_m.size(), ally_m.size(), cc_m.size());
        OutputDebugStringA(dbg);
    }

    {
        std::lock_guard<std::mutex> lk(state_mu);
        cs_role = role;
        cs_clan_id = clan;
        cs_ally_id = ally;
        cs_party_id = party;
        cs_cc_id = cc;
        cs_cc_leader_id = ccLeader;
        cs_cc_can_speak = ccCanSpeak;
        cs_clan_mode = mode;
        cs_opted_out = optedOut;
        cs_party_members = std::move(party_m);
        cs_clan_members  = std::move(clan_m);
        cs_ally_members  = std::move(ally_m);
        cs_cc_members    = std::move(cc_m);
    }
}

size_t VoiceNetwork::GetGroupMembers(uint8_t group, GroupMember* out, size_t cap) const {
    std::lock_guard<std::mutex> lk(impl_->state_mu);
    const std::vector<GroupMember>* src = nullptr;
    switch (group) {
        case 1: src = &impl_->cs_party_members; break;
        case 2: src = &impl_->cs_clan_members;  break;
        case 3: src = &impl_->cs_ally_members;  break;
        case 4: src = &impl_->cs_cc_members;    break;
        default: return 0;
    }
    size_t n = src->size();
    if (cap == 0 || out == nullptr) return n;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; ++i) out[i] = (*src)[i];
    return n;
}

uint8_t VoiceNetwork::LocalRole()         const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_role; }
uint32_t VoiceNetwork::LocalClanID()      const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_clan_id; }
uint32_t VoiceNetwork::LocalAllyID()      const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_ally_id; }
uint64_t VoiceNetwork::LocalPartyID()     const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_party_id; }
uint32_t VoiceNetwork::LocalCCID()        const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_cc_id; }
uint32_t VoiceNetwork::LocalCCLeaderID()  const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_cc_leader_id; }
bool     VoiceNetwork::LocalCCCanSpeak()  const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_cc_can_speak; }
uint8_t  VoiceNetwork::LocalClanMode()    const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->cs_clan_mode; }

void VoiceNetwork::SendPlayerNameQuery(uint32_t player_id) {
    if (!impl_->connected.load() || player_id == 0) return;
    {
        std::lock_guard<std::mutex> lk(impl_->names_mu);
        if (impl_->player_name_cache.count(player_id)) return;
        if (impl_->player_name_inflight.count(player_id)) return;
        impl_->player_name_inflight[player_id] = true;
    }
    std::string s = "{\"type\":\"player_name_query\",\"player_id\":";
    s += std::to_string(player_id);
    s += "}";
    impl_->ws.send(s);
}

bool VoiceNetwork::CachedPlayerName(uint32_t player_id, char* out, size_t cap) {
    if (cap == 0) return false;
    std::lock_guard<std::mutex> lk(impl_->names_mu);
    auto it = impl_->player_name_cache.find(player_id);
    if (it == impl_->player_name_cache.end()) { out[0] = 0; return false; }
    size_t n = it->second.size();
    if (n >= cap) n = cap - 1;
    memcpy(out, it->second.data(), n);
    out[n] = 0;
    return true;
}

bool VoiceNetwork::CachedName(uint32_t src_id, char* out, size_t cap) {
    if (cap == 0) return false;
    std::lock_guard<std::mutex> lk(impl_->names_mu);
    auto it = impl_->name_cache.find(src_id);
    if (it == impl_->name_cache.end()) { out[0] = 0; return false; }
    size_t n = it->second.size();
    if (n >= cap) n = cap - 1;
    memcpy(out, it->second.data(), n);
    out[n] = 0;
    return true;
}

}  // namespace voice
