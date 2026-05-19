// voice.cpp — orchestrator. Wires AudioCapture → OpusEncoder →
// VoiceNetwork::SendProximityFrame on capture callbacks, and
// VoiceNetwork PacketCallback → OpusDecoder → AudioPlayback::Enqueue
// on incoming packets.
//
// Proximity routing and per-receiver gain/pan happen on the service
// (protocol rev 2). The client just plays whatever gain/pan the
// service stamps onto each packet.

#include "voice.h"
#include "audio_io.h"
#include "opus_codec.h"
#include "user_hook.h"
#include "voice_network.h"

#include <windows.h>
#include <atomic>
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

    std::atomic<uint16_t> tx_seq{0};

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

void OnCaptureFrame(const int16_t* pcm, uint32_t samples) {
    if (!g_mod.running.load()) return;
    if (samples != kFrameSamples) return;

    bool ptt = false;
    if (g_mod.cfg.ptt_proximity != 0) {
        ptt = (GetAsyncKeyState(g_mod.cfg.ptt_proximity) & 0x8000) != 0;
    }
    if (!ptt) return;
    if (!g_mod.net.IsConnected()) return;

    uint8_t opus_buf[kMaxPacketBytes];
    int n = g_mod.encoder.Encode(pcm, opus_buf, sizeof(opus_buf));
    if (n <= 0) return;

    uint16_t seq = g_mod.tx_seq.fetch_add(1, std::memory_order_relaxed);
    g_mod.net.SendProximityFrame(seq, opus_buf, n);
}

void OnIncomingPacket(uint8_t channel, uint32_t src, uint16_t /*seq*/,
                      uint8_t gain_u8, int8_t pan_i8,
                      const uint8_t* opus_payload, uint16_t opus_len) {
    // Channel 0 = proximity (gain/pan supplied by service).
    // Channels 1..4 = group voice (gain=255 pan=0 by convention).
    if (channel > 4) return;
    if (channel != 0 && (channel < 1 || channel > 4)) return;

    OpusDecoder* dec = g_mod.DecoderFor(src);
    if (!dec) return;

    int16_t pcm[kFrameSamples];
    int got = dec->Decode(opus_payload, opus_len, pcm, kFrameSamples);
    if (got <= 0) return;

    float gain = (float)gain_u8 / 255.0f;
    float pan  = (float)pan_i8  / 127.0f;
    g_mod.playback.Enqueue(src, pcm, (uint32_t)got, gain, pan);
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
    c.ptt_proximity = 'H';  // V conflicts with the client's inventory hotkey
    c.ptt_party     = 'B';
    c.ptt_clan      = 'N';
    c.ptt_ally      = 'M';
    c.enabled       = true;
    c.auto_connect  = true;
    c.player_id     = 0;
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
    c.player_id      = (uint32_t)getI(L"player_id", 0);
    *out = c;
    return true;
}

bool Init(const Config& cfg) {
    if (g_mod.running.load()) return true;
    if (!cfg.enabled)         return false;

    g_mod.cfg = cfg;

    if (!g_mod.encoder.Init())                       return false;
    if (!g_mod.playback.Start(cfg.playback_device))  return false;

    if (!g_mod.capture.Start(cfg.capture_device, &OnCaptureFrame)) {
        g_mod.playback.Stop();
        return false;
    }

    if (cfg.auto_connect) {
        g_mod.net.Start(cfg.ws_url,
                        [](uint32_t /*sid*/, const char*, uint16_t) {},
                        &OnIncomingPacket);
        // Pre-arm with whatever ini/env gave us (fallback path). If the
        // engine.dll hook discovers a real ObjectId later, the callback
        // below overwrites it before the next auth attempt.
        if (cfg.player_id != 0) {
            g_mod.net.SetAuthToken("", cfg.player_id);
        }

        // Try to install the engine.dll hook for auto-detection.
        // Callback overrides the configured player_id once the local
        // player's User::SetName fires.
        InstallUserHook([](uint32_t pid) {
            g_mod.cfg.player_id = pid;
            g_mod.net.SetAuthToken("", pid);
        });
    }

    g_mod.running.store(true);
    return true;
}

void Shutdown() {
    if (!g_mod.running.exchange(false)) return;
    UninstallUserHook();
    g_mod.capture.Stop();
    g_mod.net.Stop();
    g_mod.playback.Stop();
    std::lock_guard<std::mutex> lk(g_mod.dec_mu);
    g_mod.decoders.clear();
}

void OnRenderFrame() {
    // No-op in rev 2 — position comes from the server.
    // Reserved for future HUD work (ImGui "X is speaking" badge).
}

void SetAuthToken(const char* token, uint32_t player_id) {
    g_mod.net.SetAuthToken(token, player_id);
}

bool HasActiveSpeakers() {
    return g_mod.playback.ActiveSpeakers() > 0;
}

}  // namespace voice
