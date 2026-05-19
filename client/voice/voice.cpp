// voice.cpp — orchestrator. Wires AudioCapture → OpusEncoder →
// VoiceNetwork::SendProximityFrame on capture callbacks, and
// VoiceNetwork PacketCallback → OpusDecoder → AudioPlayback::Enqueue
// on incoming packets.
//
// MVP scope: PROXIMITY ONLY. PTT proximity key (default 'V') gates
// the send path. Multibox and channel switching come later.

#include "voice.h"
#include "audio_io.h"
#include "memory_reader.h"
#include "opus_codec.h"
#include "voice_network.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace voice {

namespace {

struct Mod {
    Config cfg{};
    std::atomic<bool> running{false};

    std::atomic<bool> ptt_proximity_down{false};
    std::atomic<uint32_t> tx_seq{0};

    AudioCapture capture;
    AudioPlayback playback;
    OpusEncoder   encoder;
    VoiceNetwork  net;

    std::mutex dec_mu;
    std::unordered_map<uint32_t, std::unique_ptr<OpusDecoder>> decoders;

    OpusDecoder* DecoderFor(uint32_t src) {
        std::lock_guard<std::mutex> lk(dec_mu);
        auto it = decoders.find(src);
        if (it != decoders.end()) return it->second.get();
        auto d = std::make_unique<OpusDecoder>();
        if (!d->Init()) return nullptr;
        OpusDecoder* p = d.get();
        decoders.emplace(src, std::move(d));
        return p;
    }
};

Mod g_mod;

float Distance3D(float dx, float dy, float dz) {
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float DistanceGain(float dist, float min_dist, float max_dist) {
    if (dist <= min_dist) return 1.0f;
    if (dist >= max_dist) return 0.0f;
    return 1.0f - (dist - min_dist) / (max_dist - min_dist);
}

void OnCaptureFrame(const int16_t* pcm, uint32_t samples) {
    if (!g_mod.running.load()) return;
    if (samples != kFrameSamples) return;

    // PTT gate (Win32 GetAsyncKeyState — high bit set when pressed).
    bool ptt = false;
    if (g_mod.cfg.ptt_proximity != 0) {
        ptt = (GetAsyncKeyState(g_mod.cfg.ptt_proximity) & 0x8000) != 0;
    }
    g_mod.ptt_proximity_down.store(ptt);
    if (!ptt) return;
    if (!g_mod.net.IsConnected()) return;

    LocalPlayerState s = ReadLocalPlayerState();
    if (!s.ok) {
        // Stub mode: send zeros so the server still routes us in
        // loopback (we'll receive ourselves back when -echo is on).
        s.x = s.y = s.z = 0.0f;
        s.instance_id = 0;
    }

    uint8_t opus_buf[kMaxPacketBytes];
    int n = g_mod.encoder.Encode(pcm, opus_buf, sizeof(opus_buf));
    if (n <= 0) return;

    uint32_t seq = g_mod.tx_seq.fetch_add(1, std::memory_order_relaxed);
    g_mod.net.SendProximityFrame(seq, s.x, s.y, s.z, s.instance_id,
                                 opus_buf, n);
}

void OnIncomingPacket(uint8_t channel, uint32_t src,
                      uint32_t /*seq*/,
                      float x, float y, float z, uint32_t /*inst*/,
                      const uint8_t* opus_payload, uint16_t opus_len) {
    if (channel != 0) return;
    OpusDecoder* dec = g_mod.DecoderFor(src);
    if (!dec) return;

    int16_t pcm[kFrameSamples];
    int got = dec->Decode(opus_payload, opus_len, pcm, kFrameSamples);
    if (got <= 0) return;

    LocalPlayerState me = ReadLocalPlayerState();
    float dx = x - (me.ok ? me.x : 0.0f);
    float dy = y - (me.ok ? me.y : 0.0f);
    float dz = z - (me.ok ? me.z : 0.0f);
    float dist = Distance3D(dx, dy, dz);
    float gain = DistanceGain(dist, g_mod.cfg.min_dist_cm, g_mod.cfg.max_dist_cm);

    g_mod.playback.Enqueue(src, pcm, (uint32_t)got, dx, dy, dz, gain);
}

}  // namespace

Config DefaultConfig() {
    Config c{};
    std::strncpy(c.ws_url, "ws://127.0.0.1:17667/ws", sizeof(c.ws_url) - 1);
    c.udp_host[0]   = 0;
    c.udp_port      = 0;
    c.capture_device[0] = 0;
    c.playback_device[0] = 0;
    c.min_dist_cm   = 500.0f;
    c.max_dist_cm   = 2500.0f;
    c.ptt_proximity = 'V';
    c.ptt_party     = 'B';
    c.ptt_clan      = 'N';
    c.ptt_ally      = 'M';
    c.enabled       = true;
    c.auto_connect  = true;
    return c;
}

bool LoadConfigFromIni(const wchar_t* path, Config* out) {
    if (!path || !out) return false;
    Config c = DefaultConfig();
    wchar_t buf[512];
    auto getS = [&](const wchar_t* key, char* dst, size_t cap, const char* def) {
        GetPrivateProfileStringW(L"voice", key, L"", buf, 512, path);
        if (buf[0] == 0) { std::strncpy(dst, def, cap - 1); return; }
        size_t n = 0;
        wcstombs_s(&n, dst, cap, buf, cap - 1);
    };
    auto getI = [&](const wchar_t* key, int def) -> int {
        return GetPrivateProfileIntW(L"voice", key, def, path);
    };
    getS(L"ws_url", c.ws_url, sizeof(c.ws_url), c.ws_url);
    getS(L"capture_device", c.capture_device, sizeof(c.capture_device), "");
    getS(L"playback_device", c.playback_device, sizeof(c.playback_device), "");
    c.min_dist_cm    = (float)getI(L"min_dist_cm", (int)c.min_dist_cm);
    c.max_dist_cm    = (float)getI(L"max_dist_cm", (int)c.max_dist_cm);
    c.ptt_proximity  = getI(L"ptt_proximity",  c.ptt_proximity);
    c.ptt_party      = getI(L"ptt_party",      c.ptt_party);
    c.ptt_clan       = getI(L"ptt_clan",       c.ptt_clan);
    c.ptt_ally       = getI(L"ptt_ally",       c.ptt_ally);
    c.enabled        = getI(L"enabled", 1) != 0;
    c.auto_connect   = getI(L"auto_connect", 1) != 0;
    *out = c;
    return true;
}

bool Init(const Config& cfg) {
    if (g_mod.running.load()) return true;
    if (!cfg.enabled)         return false;

    g_mod.cfg = cfg;

    if (!g_mod.encoder.Init())   return false;
    if (!g_mod.playback.Start(cfg.playback_device)) return false;
    g_mod.playback.SetAttenuation(cfg.min_dist_cm, cfg.max_dist_cm);

    if (!g_mod.capture.Start(cfg.capture_device, &OnCaptureFrame)) {
        g_mod.playback.Stop();
        return false;
    }

    if (cfg.auto_connect) {
        g_mod.net.Start(cfg.ws_url,
                        [](uint32_t /*sid*/, const char*, uint16_t) {},
                        &OnIncomingPacket);
    }

    g_mod.running.store(true);
    return true;
}

void Shutdown() {
    if (!g_mod.running.exchange(false)) return;
    g_mod.capture.Stop();
    g_mod.net.Stop();
    g_mod.playback.Stop();
    std::lock_guard<std::mutex> lk(g_mod.dec_mu);
    g_mod.decoders.clear();
}

void OnRenderFrame() {
    // Refresh the cached local-player state once per frame, so the
    // next 20ms capture frame stamps fresh coordinates.
    RefreshLocalPlayerState();
    // HUD ImGui panel: deferred — not blocking the loopback test.
}

void SetAuthToken(const char* token, uint32_t player_id) {
    g_mod.net.SetAuthToken(token, player_id);
}

bool HasActiveSpeakers() {
    return g_mod.playback.ActiveSpeakers() > 0;
}

}  // namespace voice
