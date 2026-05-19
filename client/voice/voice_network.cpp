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
#include <thread>
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

}  // namespace

struct VoiceNetwork::Impl {
    std::string ws_url;
    std::mutex auth_mu;
    std::vector<uint16_t> client_ports;

    std::atomic<uint32_t> session_id{0};
    std::string udp_host;
    std::atomic<uint16_t> udp_port{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> auth_sent{false};

    AuthOkCallback on_auth;
    PacketCallback on_packet;

    SOCKET udp_sock = INVALID_SOCKET;
    sockaddr_in udp_dest{};
    ix::WebSocket ws;
    std::thread udp_thread;
    std::atomic<bool> stopping{false};

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

            if (channel == 0) {                 // proximity, 10-byte header
                if (n < 10) continue;
                gain = buf[8];
                pan  = (int8_t)buf[9];
                opus = buf + 10;
                opus_len = (uint16_t)(n - 10);
            } else if (channel >= 1 && channel <= 4) {  // party/clan/ally/siege
                opus = buf + 8;
                opus_len = (uint16_t)(n - 8);
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
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    connected.store(true);
                    auth_sent.store(false);
                    TrySendAuth();
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    connected.store(false);
                    auth_sent.store(false);
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
        if (s.find("\"auth_ok\"") == std::string::npos) return;
        uint64_t sid_u = 0;
        if (!ExtractNumber(s, "session_id", sid_u)) return;
        std::string host;
        uint64_t port_u = 0;
        if (!ExtractString(s, "udp_host", host)) return;
        if (!ExtractNumber(s, "udp_port", port_u)) return;
        session_id.store((uint32_t)sid_u);
        udp_host = host;
        udp_port.store((uint16_t)port_u);
        SetUdpDest(host, (uint16_t)port_u);
        if (on_auth) on_auth((uint32_t)sid_u, host.c_str(), (uint16_t)port_u);
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
    {
        std::lock_guard<std::mutex> lk(impl_->auth_mu);
        impl_->client_ports.assign(ports, ports + count);
        impl_->auth_sent.store(false);
    }
    impl_->TrySendAuth();
}

void VoiceNetwork::SendProximityFrame(uint16_t seq,
                                      const uint8_t* opus, int opus_len) {
    if (impl_->udp_port.load() == 0) return;
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

}  // namespace voice
