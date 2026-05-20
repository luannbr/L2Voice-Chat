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
};

OverlayState SnapshotOverlayState();

// Setters back into the voice module (so the overlay can mutate
// config at runtime). Each is thread-safe (atomic where simple,
// mutex'd where compound).
void SetRequireFocus(bool v);
void SetAlwaysOn(bool v);
void SetPttProximityVk(int vk);
void SetMasterVolume(float gain);

// Speaker list for the overlay. SpeakerInfo lives in audio_io.h.
struct SpeakerInfo;
void GetSpeakerList(SpeakerInfo* out, size_t cap, size_t& count);
void SetSpeakerMuted(uint32_t src_id, bool muted);
void SetSpeakerVolume(uint32_t src_id, float volume);

// Resolves the character name for a speaker. Returns true + fills out
// when known, false otherwise. Triggers an async query the first time
// — the cache fills in within ~50 ms typically.
bool GetSpeakerName(uint32_t src_id, char* out, size_t cap);

}  // namespace voice
