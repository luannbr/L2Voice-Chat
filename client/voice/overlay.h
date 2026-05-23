// overlay.h — in-game ImGui panel for l2voice.
//
// Hooks IDirect3DDevice9::EndScene via MinHook (using the well-known
// "dummy device" trick to find the vtable address). On first frame
// after L2's d3d9 device is up, initializes ImGui + the Win32 input
// hook, then renders a draggable settings panel every frame.
//
// Toggle visibility with Insert (default; configurable via voice.ini
// `overlay_toggle_vk`).

#pragma once

#include <cstdint>

namespace voice {

// Install the D3D9 hook + ImGui context. Idempotent. Safe to call
// from voice::Init. Logs progress via OutputDebugString.
bool InstallOverlay();

// Tear down. Detaches MinHook, releases ImGui DX9 resources, restores
// the original WndProc.
void UninstallOverlay();

// Optional: from outside (voice.cpp), pass current state to the
// overlay so it can render the right thing. Cheap accessors —
// implemented in voice.cpp using g_mod.
//
// (Defined here so overlay.cpp doesn't have to pull voice.cpp's
// internal Mod type into a header.)
struct OverlayState {
    bool     ws_connected;
    uint32_t session_id;
    uint32_t player_id;
    int      active_speakers;
    bool     require_focus;
    bool     always_on;
    int      ptt_proximity_vk;
    float    master_volume;     // 0..2, default 1.0
    // Per-channel listening preferences. Index 0..3 = Proximity / Party
    // / Clan / Ally. Send-side toggles are independent (you can hear
    // a channel disabled and still transmit on PTT, per Prompt §Regra 1).
    bool     ch_enabled[4];
    float    ch_volume[4];      // 0..2
};

OverlayState SnapshotOverlayState();

// Setters back into the voice module (so the overlay can mutate
// config at runtime). Each is thread-safe (atomic where simple,
// mutex'd where compound).
void SetRequireFocus(bool v);
void SetAlwaysOn(bool v);
void SetPttProximityVk(int vk);
void SetPttPartyVk(int vk);
void SetPttClanVk(int vk);
void SetPttAllyVk(int vk);
int  GetPttPartyVk();
int  GetPttClanVk();
int  GetPttAllyVk();
void SetMasterVolume(float gain);

// Per-channel listening prefs (Prompt §Fase C wire: set_channel_*).
// Updates the local state, persists to voice.ini, and forwards to the
// voice-service so its routing decisions reflect the change.
// `channel` is 0..3 (Proximity/Party/Clan/Ally).
void SetChannelEnabled(int channel, bool enabled);
void SetChannelVolume(int channel, float volume);

// Active transmit channel for PTT and always-on. 0..4 (Proximity/Party/
// Clan/Ally/CC). When the user holds the PTT key (or always-on is set),
// captured audio is sent on this channel. Legacy per-channel PTT keys
// still override when held. Persists to voice.ini.
void SetActiveTxChannel(int channel);
int  GetActiveTxChannel();

// Round-trip latency to the voice-service in ms (WS ping/pong). -1 when
// no reply has arrived yet (just-connected or disconnected).
int  GetVoicePingMs();

// Persisted UI element positions. Each pair has its own key in voice.ini.
// Getters return 0/0 when no value is stored. Setters write to voice.ini.
void GetMiniListPos(int* x, int* y);
void SaveMiniListPos(int x, int y);
void GetMicIconPos(int* x, int* y);
void SaveMicIconPos(int x, int y);

// Speaker list for the overlay. SpeakerInfo lives in audio_io.h.
struct SpeakerInfo;
void GetSpeakerList(SpeakerInfo* out, size_t cap, size_t& count);
void SetSpeakerMuted(uint32_t src_id, bool muted);
void SetSpeakerVolume(uint32_t src_id, float volume);

// Resolves the character name for a speaker. Returns true + fills out
// when known, false otherwise. Triggers an async query the first time
// — the cache fills in within ~50 ms typically.
bool GetSpeakerName(uint32_t src_id, char* out, size_t cap);

// Per-group member roster pushed by the server via client_state.
struct OverlayMember {
    uint32_t player_id;
    bool     voice_active;
    uint8_t  clan_role;     // clan tab only (0=member, 1=sub, 2=leader)
    bool     cc_can_speak;  // CC tab only
    bool     remote_muted;  // clan/ally only — server-pushed
};
// `group` is 1..4 (Party/Clan/Ally/CC). Returns count written.
size_t GetGroupRoster(uint8_t group, OverlayMember* out, size_t cap);

// Resolves character name for an L2 player_id (separate from src_id
// based name lookup which is keyed on session ids).
bool GetPlayerName(uint32_t player_id, char* out, size_t cap);

// CC state for conditional rendering of the CC tab + leader controls.
uint32_t GetCCID();
uint32_t GetCCLeaderID();
bool     GetCCCanSpeak();
uint8_t  GetLocalRole();

// Current clan operational mode pushed by the server in client_state.
// 0=None, 1=PVP, 2=Siege, 3=Boss, 4=Farm. The overlay uses this to
// render the top-of-screen mode banner.
uint8_t  GetLocalClanMode();

// Toast notifications produced by server-pushed events
// (remote-mute, mode changes, etc.). Layout-stable POD.
struct OverlayToast {
    uint32_t id;
    char     text[192];
    uint8_t  severity;     // 0=info, 1=warn, 2=error, 3=success
    int64_t  created_ms;
    int64_t  dismiss_ms;
};
size_t GetActiveToasts(OverlayToast* out, size_t cap);

// Send CC grant-speak.
void SendCCGrantSpeak(uint32_t target_player_id, bool granted);
// Send remote mute on clan/ally scope.
void SendClanRemoteMute(uint32_t target_player_id, bool muted, const char* scope);
// Send clan mode change.
void SendClanSetMode(uint8_t mode);
// Promote / demote sub-leader.
void SendClanPromoteSub(uint32_t target_player_id, bool promote);

}  // namespace voice
