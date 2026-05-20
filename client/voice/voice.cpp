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
#include "overlay.h"
#include "voice_network.h"

#include <winsock2.h>      // must precede iphlpapi.h
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace voice {

namespace {

struct Mod {
    Config cfg{};
    std::atomic<bool> running{false};
    // Path to voice.ini next to the DLL — captured at Init so the
    // setters can write changes back without re-resolving each time.
    wchar_t ini_path[MAX_PATH] = {};

    std::atomic<uint16_t> tx_seq{0};

    AudioCapture capture;
    AudioPlayback playback;
    OpusEncoder   encoder;
    VoiceNetwork  net;
    std::thread   keepalive_thread;

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

// True iff a window owned by THIS process currently has keyboard focus.
// Used to gate microphone capture so holding the PTT key while the L2
// window is minimized / Alt-Tab'd doesn't transmit anything.
bool L2HasForegroundFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == GetCurrentProcessId();
}

void OnCaptureFrame(const int16_t* pcm, uint32_t samples) {
    if (!g_mod.running.load()) return;
    if (samples != kFrameSamples) return;

    bool focused = !g_mod.cfg.require_focus || L2HasForegroundFocus();
    bool ptt_pressed = false;
    if (g_mod.cfg.ptt_proximity != 0) {
        ptt_pressed = (GetAsyncKeyState(g_mod.cfg.ptt_proximity) & 0x8000) != 0;
    }
    // Transmit decision:
    //   always_on  → talk whenever the L2 window is focused
    //   PTT mode   → talk only when the L2 window is focused AND PTT is held
    // require_focus=0 disables the focus check (useful for testing only).
    bool transmit = focused && (g_mod.cfg.always_on || ptt_pressed);

    static uint32_t cap_frames = 0;
    if ((++cap_frames % 50) == 0) {
        char dbg[200];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] capture=%u tx=%d focus=%d ptt=%d on=%d ws=%d sid=%u\n",
            cap_frames, transmit ? 1 : 0, focused ? 1 : 0,
            ptt_pressed ? 1 : 0, g_mod.cfg.always_on ? 1 : 0,
            g_mod.net.IsConnected() ? 1 : 0, g_mod.net.SessionID());
        OutputDebugStringA(dbg);
    }

    if (!transmit) return;
    if (!g_mod.net.IsConnected()) return;

    uint8_t opus_buf[kMaxPacketBytes];
    int n = g_mod.encoder.Encode(pcm, opus_buf, sizeof(opus_buf));
    if (n <= 0) return;

    static uint32_t sent = 0;
    if ((++sent % 50) == 1) {  // every ~1s of speech
        char dbg[96];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] SendProximityFrame #%u (%d bytes opus)\n", sent, n);
        OutputDebugStringA(dbg);
    }
    uint16_t seq = g_mod.tx_seq.fetch_add(1, std::memory_order_relaxed);
    g_mod.net.SendProximityFrame(seq, opus_buf, n);
}

void OnIncomingPacket(uint8_t channel, uint32_t src, uint16_t /*seq*/,
                      uint8_t gain_u8, int8_t pan_i8,
                      const uint8_t* opus_payload, uint16_t opus_len) {
    // Make sure we have a name on file for the speaker so the overlay
    // can label them. Cheap — VoiceNetwork dedupes inflight queries
    // and cached names skip the WS roundtrip.
    g_mod.net.SendNameQuery(src);

    static uint32_t rx = 0;
    if ((++rx % 50) == 1) {
        char dbg[160];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "[l2voice] recv #%u ch=%u src=%u gain=%u pan=%d opus=%u\n",
            rx, channel, src, gain_u8, pan_i8, opus_len);
        OutputDebugStringA(dbg);
    }
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
    c.require_focus = true;
    c.always_on     = false;
    c.master_volume = 1.0f;
    return c;
}


// Enumerates this process's owned TCP connections and returns the local
// (source) ports. The bridge will match these against L2GameClient
// remote ports to figure out which player this DLL belongs to.
//
// We skip the loopback (127.0.0.1) side and the WS port itself so the
// list doesn't include the voice-server connection. Everything else is
// fair game — bridge knows which port is the game socket.
std::vector<uint16_t> EnumerateOwnTcpPorts(uint16_t exclude_remote_port) {
    std::vector<uint16_t> out;
    DWORD pid = GetCurrentProcessId();
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                        TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return out;
    std::vector<uint8_t> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return out;
    }
    auto* tbl = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& r = tbl->table[i];
        if (r.dwOwningPid != pid) continue;
        uint16_t localPort  = ntohs((u_short)r.dwLocalPort);
        uint16_t remotePort = ntohs((u_short)r.dwRemotePort);
        if (remotePort == exclude_remote_port) continue;
        // Only ESTABLISHED connections — listening sockets don't have
        // a meaningful "remote" the GS knows about.
        if (r.dwState != MIB_TCP_STATE_ESTAB) continue;
        out.push_back(localPort);
    }
    return out;
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
    c.require_focus  = getI(L"require_focus", 1) != 0;
    c.always_on      = getI(L"always_on", 0) != 0;
    c.master_volume  = (float)getI(L"master_volume", 100) / 100.0f;
    *out = c;
    return true;
}

void RefreshClientPorts();   // fwd decl

// Used by dllmain.cpp to tell us where voice.ini lives (so the
// overlay setters can persist changes). Call once before Init.
void SetIniPath(const wchar_t* path) {
    if (!path) { g_mod.ini_path[0] = 0; return; }
    wcsncpy_s(g_mod.ini_path, MAX_PATH, path, MAX_PATH - 1);
}

// Writes an integer key to [voice] in voice.ini. Cheap: WinAPI does
// the parse/replace/write internally.
static void IniWriteInt(const wchar_t* key, int value) {
    if (g_mod.ini_path[0] == 0) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(L"voice", key, buf, g_mod.ini_path);
}

bool Init(const Config& cfg) {
    if (g_mod.running.load()) return true;
    if (!cfg.enabled)         return false;

    g_mod.cfg = cfg;

    if (!g_mod.encoder.Init())                       return false;
    if (!g_mod.playback.Start(cfg.playback_device))  return false;
    g_mod.playback.SetMasterVolume(cfg.master_volume);

    if (!g_mod.capture.Start(cfg.capture_device, &OnCaptureFrame)) {
        g_mod.playback.Stop();
        return false;
    }

    if (cfg.auto_connect) {
        g_mod.net.Start(cfg.ws_url,
                        [](uint32_t /*sid*/, const char*, uint16_t) {},
                        &OnIncomingPacket);
        // Identity is resolved server-side via TCP source-port matching.
        // The bridge correlates this DLL's local-port list against
        // L2GameClient remote ports → returns the player_id. We push an
        // initial snapshot here and refresh on each WS reconnect (see
        // OnRenderFrame).
        RefreshClientPorts();
    }

    g_mod.running.store(true);

    // Install the in-game overlay (D3D9 EndScene hook + ImGui panel).
    // Logs progress to OutputDebugString — failures are non-fatal,
    // the audio pipeline still works without the UI.
    if (!InstallOverlay()) {
        OutputDebugStringA("[l2voice] overlay install failed; continuing without UI\n");
    }

    // Keepalive thread: every 5s the DLL emits a UDP header-only
    // packet so the voice-service learns (and refreshes) our UDP
    // source address. WITHOUT this, the service only knows the UDP
    // addrs of clients that are actively transmitting audio — so
    // listeners-who-haven't-spoken-yet never receive anything.
    g_mod.keepalive_thread = std::thread([] {
        using namespace std::chrono;
        while (g_mod.running.load(std::memory_order_acquire)) {
            for (int i = 0; i < 10 && g_mod.running.load(); ++i) {
                std::this_thread::sleep_for(milliseconds(500));
            }
            if (g_mod.net.IsConnected() && g_mod.net.SessionID() != 0) {
                g_mod.net.SendKeepalive();
            }
        }
    });
    return true;
}

void RefreshClientPorts() {
    // Exclude the WS connection itself (port 17667 by default). We
    // could parse cfg.ws_url for the actual port, but 17667 is the
    // default and adding a few extras to the list is harmless.
    auto ports = EnumerateOwnTcpPorts(/*exclude_remote_port*/ 17667);
    char dbg[128];
    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
        "[l2voice] RefreshClientPorts: %zu connections\n", ports.size());
    OutputDebugStringA(dbg);
    if (ports.empty()) return;
    g_mod.net.SetClientPorts(ports.data(), ports.size());
}

void Shutdown() {
    if (!g_mod.running.exchange(false)) return;
    UninstallOverlay();
    if (g_mod.keepalive_thread.joinable()) g_mod.keepalive_thread.join();
    g_mod.capture.Stop();
    g_mod.net.Stop();
    g_mod.playback.Stop();
    std::lock_guard<std::mutex> lk(g_mod.dec_mu);
    g_mod.decoders.clear();
}

OverlayState SnapshotOverlayState() {
    OverlayState s{};
    s.ws_connected     = g_mod.net.IsConnected();
    s.session_id       = g_mod.net.SessionID();
    s.player_id        = g_mod.net.PlayerID();
    s.active_speakers  = g_mod.playback.ActiveSpeakers();
    s.require_focus    = g_mod.cfg.require_focus;
    s.always_on        = g_mod.cfg.always_on;
    s.ptt_proximity_vk = g_mod.cfg.ptt_proximity;
    s.master_volume    = g_mod.playback.GetMasterVolume();
    return s;
}

// Setters persist to voice.ini so the user's preferences survive
// the next L2 launch. Master volume is stored as a percent (0..200).
void SetRequireFocus(bool v) {
    g_mod.cfg.require_focus = v;
    IniWriteInt(L"require_focus", v ? 1 : 0);
}
void SetAlwaysOn(bool v) {
    g_mod.cfg.always_on = v;
    IniWriteInt(L"always_on", v ? 1 : 0);
}
void SetPttProximityVk(int vk) {
    g_mod.cfg.ptt_proximity = vk;
    IniWriteInt(L"ptt_proximity", vk);
}
void SetMasterVolume(float g) {
    g_mod.playback.SetMasterVolume(g);
    IniWriteInt(L"master_volume", (int)(g * 100.0f + 0.5f));
}
void GetSpeakerList(SpeakerInfo* out, size_t cap, size_t& count) {
    g_mod.playback.GetSpeakerInfos(out, cap, count);
}
void SetSpeakerMuted(uint32_t src_id, bool muted) {
    g_mod.playback.SetSourceMuted(src_id, muted);
}

bool GetSpeakerName(uint32_t src_id, char* out, size_t cap) {
    // Side-effect: kick off a query if we don't have it yet.
    g_mod.net.SendNameQuery(src_id);
    return g_mod.net.CachedName(src_id, out, cap);
}

void OnRenderFrame() {
    // Refresh the TCP-port list periodically — by the time the user
    // is in-world, their L2 client has opened the GS socket; before
    // that there's nothing useful to send. Cheap (one syscall).
    static int counter = 0;
    if ((counter++ % 60) == 0) RefreshClientPorts();
}

void SetAuthToken(const char* /*token*/, uint32_t /*player_id*/) {
    // Deprecated — identity is resolved server-side via TCP source-
    // port matching now. Left as a no-op so the public API doesn't
    // break callers that wired it up (none today).
}

bool HasActiveSpeakers() {
    return g_mod.playback.ActiveSpeakers() > 0;
}

}  // namespace voice
