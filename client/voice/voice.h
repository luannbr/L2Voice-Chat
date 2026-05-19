// voice.h — Public API of the voice module inside l2voice.dll
// (standalone, separate from l2ui/AutoLogin).
//
// Architecture (proximity MVP):
//
//   ┌── voice::Init() ────────────────────────────────────────────────┐
//   │  1. Start AudioCapture (miniaudio WASAPI, 48kHz mono 20ms)      │
//   │  2. Start AudioPlayback (miniaudio mixer with 3D attenuation)   │
//   │  3. Create OpusEncoder + OpusDecoder pool (one decoder per src) │
//   │  4. Open VoiceNetwork: WS auth → UDP socket bound               │
//   │  5. Spawn capture+send thread + recv+play thread                │
//   └─────────────────────────────────────────────────────────────────┘
//
// On every captured 20ms frame:
//   - if PTT held and channel == proximity:
//       MemoryReader::ReadLocalPlayerXYZInstance(&x,&y,&z,&inst)
//       opus encode → build proximity packet → UDP send
//
// On every received packet:
//   - parse header
//   - lookup or create decoder for sender's session_id
//   - opus decode → enqueue PCM into mixer with sender's reported xyz
//   - mixer applies linear attenuation between min_dist and max_dist
//
// All state is in this namespace; nothing exported to other modules
// except voice::Init/Shutdown/OnRenderFrame (the latter draws the
// overlay HUD panel via ImGui).

#pragma once

#include <cstdint>
#include <windows.h>

namespace voice {

// Channels match the wire protocol enum.
enum class Channel : uint8_t {
    Proximity = 0,
    Party     = 1,
    Clan      = 2,
    Ally      = 3,
    // Internal types not surfaced here: Siege/Keepalive/Ping.
};

// Configuration loaded from voice.ini (sibling of l2ui.dll). All
// fields have sensible defaults so the file can be missing.
struct Config {
    // Server endpoints.
    char     ws_url[256];        // e.g. "ws://127.0.0.1:17667/ws"
    char     udp_host[128];      // populated from auth_ok response
    uint16_t udp_port;           // populated from auth_ok response

    // Audio.
    char     capture_device[128]; // "" = default; matches miniaudio device names
    char     playback_device[128];

    // Proximity attenuation.
    float    min_dist_cm;        // default 500.0 (5 m, full volume below)
    float    max_dist_cm;        // default 2500.0 (25 m, zero volume above)

    // PTT keys (Win32 VK_*). 0 = disabled.
    int      ptt_proximity;      // default 'V'
    int      ptt_party;          // default 'B'
    int      ptt_clan;           // default 'N'
    int      ptt_ally;           // default 'M'

    // Master flags.
    bool     enabled;            // false = module loaded but inert
    bool     auto_connect;       // try to auth WS at startup

    // TEMP — task #29: replace with a g_localUser->ObjectId memory
    // read once the offset is mapped. For now, voice.ini carries the
    // player_id so the DLL can pass it on auth. This is NOT a
    // credential — the voice-service still validates against the L2J
    // bridge (/voice/check). An attacker who edits this file just
    // claims to be a player; if that player isn't online in L2J, the
    // session is refused.
    uint32_t player_id;
};

Config DefaultConfig();
bool LoadConfigFromIni(const wchar_t* path, Config* out);

// Init/Shutdown — called from the existing OverlayInstall path in
// dllmain.cpp, behind a feature flag in Config.
//
// Init is async-safe to call once. It spawns its own threads. Returns
// true if at least the audio devices opened successfully; UDP/WS will
// keep retrying in the background.
bool Init(const Config& cfg);
void Shutdown();

// Called every frame from RenderFrame (d3d9_hook.cpp). Tiny: polls
// hotkey state, repaints the HUD panel via ImGui if visible.
void OnRenderFrame();

// Called when the L2 token has been obtained (Phase 4 will hook into
// L2J's chat broadcast / packet path). For now this is a manual entry
// in the overlay settings UI.
void SetAuthToken(const char* token, uint32_t player_id);

// Returns true if at least one remote session is currently audible.
// Used by the HUD to show the "X is speaking" badge.
bool HasActiveSpeakers();

}  // namespace voice
