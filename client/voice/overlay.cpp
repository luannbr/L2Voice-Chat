// overlay.cpp — D3D9 EndScene hook + Dear ImGui in-game panel.
//
// Hooks (per the dummy-device vtable trick):
//   - IDirect3DDevice9::EndScene  → ImGui frame
//   - IDirect3DDevice9::Reset     → ImGui device-objects invalidation
// Input routing:
//   - WndProc swap chains ImGui's input handler. Mouse / keyboard
//     messages ImGui wants are CONSUMED (don't leak to the game).
//   - WM_SETCURSOR is suppressed while ImGui has the mouse so the
//     game's custom cursor doesn't fight the panel's.
//   - A WH_MOUSE_LL low-level hook blocks mouse events at the OS
//     level when the cursor is over our window AND ImGui wants the
//     mouse — this catches DirectInput-based games (L2 included)
//     that bypass the regular WndProc path.

#include "overlay.h"
#include "audio_io.h"
#include "voice.h"

#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <MinHook.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#include <stb_image.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace voice {

namespace {

using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
using Reset_t    = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

EndScene_t      g_origEndScene = nullptr;
Reset_t         g_origReset    = nullptr;
WNDPROC         g_origWndProc  = nullptr;
HWND            g_targetHwnd   = nullptr;
IDirect3DTexture9* g_micTexture = nullptr;
int             g_micW = 0, g_micH = 0;
ImGuiContext*   g_imguiCtx     = nullptr;
std::atomic<bool> g_imguiBackendInit{false};
std::atomic<bool> g_visible{true};
std::atomic<bool> g_minimized{false};
std::atomic<bool> g_imguiCapturesMouse{false};  // sampled each frame; read by input hooks
int             g_toggleVk      = VK_INSERT;
std::atomic<bool> g_captureNextKey{false};

// Forward-declared so the DI hooks below (which sit above the Helpers
// section) can call into the same logger as everything else.
void Logf(const char* fmt, ...);

// GetAsyncKeyState hook — when ImGui's panel has the mouse focus,
// return 0 for the mouse-button VKs so L2's polling-based input
// (DirectInput-style: check GetAsyncKeyState every frame and act on
// the click) doesn't see the click that's meant for our UI.
using PFN_GetAsyncKeyState = SHORT (WINAPI*)(int);
PFN_GetAsyncKeyState g_origGetAsyncKeyState = nullptr;

SHORT WINAPI HookGetAsyncKeyState(int vk) {
    if (g_imguiCapturesMouse.load(std::memory_order_relaxed)) {
        switch (vk) {
            case VK_LBUTTON:
            case VK_RBUTTON:
            case VK_MBUTTON:
            case VK_XBUTTON1:
            case VK_XBUTTON2:
                return 0;
        }
    }
    if (g_origGetAsyncKeyState) return g_origGetAsyncKeyState(vk);
    return 0;
}

// ---- DirectInput8 hooks ---------------------------------------------
//
// L2 reads mouse buttons via IDirectInputDevice8 (Unreal Engine 2's
// standard input path). That bypasses both our WndProc consume AND
// our GetAsyncKeyState hook — the kernel still buffers mouse data
// for DirectInput regardless of message processing.
//
// Approach: hook the *shared* vtable entries for GetDeviceState +
// GetDeviceData on the SysMouse device. When ImGui captures the
// mouse, scrub button data out of the result so the game sees zero
// button presses. Mouse movement (axis data) is left alone so the
// cursor still tracks normally.

using PFN_DI_CreateDevice = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using PFN_DI_GetDeviceState = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInputDevice8A*, DWORD, LPVOID);
using PFN_DI_GetDeviceData = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

PFN_DI_CreateDevice    g_origCreateDevice = nullptr;
PFN_DI_GetDeviceState  g_origGetDeviceState = nullptr;
PFN_DI_GetDeviceData   g_origGetDeviceData = nullptr;
std::atomic<bool>      g_diMouseHooked{false};

HRESULT STDMETHODCALLTYPE HookDIGetDeviceState(
        IDirectInputDevice8A* dev, DWORD size, LPVOID data) {
    HRESULT hr = g_origGetDeviceState(dev, size, data);
    if (FAILED(hr) || !data) return hr;
    if (!g_imguiCapturesMouse.load(std::memory_order_relaxed)) return hr;
    // Zero button bytes. DIMOUSESTATE2 = lX/lY/lZ then 8 rgbButtons;
    // DIMOUSESTATE = 4 rgbButtons. Layout: button bytes start at +12.
    if (size >= sizeof(DIMOUSESTATE2)) {
        memset(&reinterpret_cast<DIMOUSESTATE2*>(data)->rgbButtons[0],
               0, 8);
    } else if (size >= sizeof(DIMOUSESTATE)) {
        memset(&reinterpret_cast<DIMOUSESTATE*>(data)->rgbButtons[0],
               0, 4);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDIGetDeviceData(
        IDirectInputDevice8A* dev, DWORD size,
        LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    HRESULT hr = g_origGetDeviceData(dev, size, data, count, flags);
    if (FAILED(hr) || !count || !data) return hr;
    if (!g_imguiCapturesMouse.load(std::memory_order_relaxed)) return hr;
    // Drop button events (rgbButtons offsets) from the buffered data.
    DWORD writeIdx = 0;
    for (DWORD i = 0; i < *count; ++i) {
        DWORD ofs = data[i].dwOfs;
        if (ofs >= DIMOFS_BUTTON0 && ofs <= DIMOFS_BUTTON0 + 7) continue;
        if (writeIdx != i) data[writeIdx] = data[i];
        ++writeIdx;
    }
    *count = writeIdx;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookDICreateDevice(
        IDirectInput8A* di, REFGUID rguid,
        LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN unk) {
    HRESULT hr = g_origCreateDevice(di, rguid, dev, unk);
    if (FAILED(hr) || !dev || !*dev) return hr;
    if (rguid == GUID_SysMouse &&
            !g_diMouseHooked.exchange(true)) {
        void** vt = *reinterpret_cast<void***>(*dev);
        // vtable indices on IDirectInputDevice8: 9=GetDeviceState,
        // 10=GetDeviceData.
        void* gs  = vt[9];
        void* gd  = vt[10];
        Logf("[l2voice] hooking IDirectInputDevice8 vt: GetDeviceState=%p GetDeviceData=%p\n", gs, gd);
        if (MH_CreateHook(gs,
                reinterpret_cast<void*>(&HookDIGetDeviceState),
                reinterpret_cast<void**>(&g_origGetDeviceState)) == MH_OK) {
            MH_EnableHook(gs);
        }
        if (MH_CreateHook(gd,
                reinterpret_cast<void*>(&HookDIGetDeviceData),
                reinterpret_cast<void**>(&g_origGetDeviceData)) == MH_OK) {
            MH_EnableHook(gd);
        }
    }
    return hr;
}

void InstallDirectInputHook() {
    HMODULE dinput8 = GetModuleHandleA("dinput8.dll");
    if (!dinput8) dinput8 = LoadLibraryA("dinput8.dll");
    if (!dinput8) {
        Logf("[l2voice] dinput8.dll not loaded — DI hook skipped\n");
        return;
    }
    using PFN_Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto pCreate = reinterpret_cast<PFN_Create>(
        GetProcAddress(dinput8, "DirectInput8Create"));
    if (!pCreate) return;
    IDirectInput8A* di = nullptr;
    HRESULT hr = pCreate(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                         IID_IDirectInput8A, (void**)&di, nullptr);
    if (FAILED(hr) || !di) {
        Logf("[l2voice] DirectInput8Create failed: %08lx\n", hr);
        return;
    }
    void** vt = *reinterpret_cast<void***>(di);
    void* createDevAddr = vt[3];   // IDirectInput8::CreateDevice
    di->Release();
    Logf("[l2voice] hooking IDirectInput8::CreateDevice=%p\n", createDevAddr);
    if (MH_CreateHook(createDevAddr,
            reinterpret_cast<void*>(&HookDICreateDevice),
            reinterpret_cast<void**>(&g_origCreateDevice)) == MH_OK) {
        MH_EnableHook(createDevAddr);
    }
}

// =============================================================
// Helpers
// =============================================================

void Logf(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// Loads voice-recorder.png from the directory the DLL itself was
// loaded from, decodes it via stb_image, and uploads to a managed
// D3D9 texture. Returns the texture pointer (and out w/h) on
// success; nullptr on any failure (file missing, decode error, GPU
// upload fail) — the minimize state silently falls back to text.
IDirect3DTexture9* LoadPngAsTexture(IDirect3DDevice9* dev,
        const wchar_t* path, int& w, int& h) {
    // Read the file ourselves so stb_image (which is ASCII-path only)
    // doesn't choke on Unicode paths.
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(f); return nullptr;
    }
    std::vector<unsigned char> buf((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(f);
    if (!ok || read != buf.size()) return nullptr;

    int c = 0;
    unsigned char* px = stbi_load_from_memory(buf.data(), (int)buf.size(),
        &w, &h, &c, 4);
    if (!px) return nullptr;

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &tex, nullptr))) {
        stbi_image_free(px);
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release(); stbi_image_free(px); return nullptr;
    }
    // stb_image gives RGBA; D3DFMT_A8R8G8B8 wants BGRA in memory.
    for (int y = 0; y < h; ++y) {
        unsigned char* src = px + y * w * 4;
        unsigned char* dst = (unsigned char*)lr.pBits + y * lr.Pitch;
        for (int x = 0; x < w; ++x) {
            dst[x*4 + 0] = src[x*4 + 2];   // B
            dst[x*4 + 1] = src[x*4 + 1];   // G
            dst[x*4 + 2] = src[x*4 + 0];   // R
            dst[x*4 + 3] = src[x*4 + 3];   // A
        }
    }
    tex->UnlockRect(0);
    stbi_image_free(px);
    return tex;
}

// Builds the absolute path to a file in the same directory as this
// DLL (resolved via GetModuleHandleEx on a function in our module).
void ResolveDllRelativePath(const wchar_t* name,
        wchar_t* out, size_t cap) {
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadPngAsTexture), &self);
    GetModuleFileNameW(self, out, (DWORD)cap);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
    wcsncat_s(out, cap, name, _TRUNCATE);
}

void VkToString(int vk, char* out, size_t cap) {
    if (cap == 0) return;
    const char* fixed = nullptr;
    switch (vk) {
        case VK_LBUTTON:  fixed = "Mouse L"; break;
        case VK_RBUTTON:  fixed = "Mouse R"; break;
        case VK_MBUTTON:  fixed = "Mouse M"; break;
        case VK_XBUTTON1: fixed = "Mouse 4"; break;
        case VK_XBUTTON2: fixed = "Mouse 5"; break;
        case VK_TAB:      fixed = "Tab"; break;
        case VK_CAPITAL:  fixed = "CapsLock"; break;
        case VK_SPACE:    fixed = "Space"; break;
        case VK_INSERT:   fixed = "Insert"; break;
        case VK_HOME:     fixed = "Home"; break;
        case VK_END:      fixed = "End"; break;
        case VK_PRIOR:    fixed = "PgUp"; break;
        case VK_NEXT:     fixed = "PgDn"; break;
        case VK_OEM_3:    fixed = "`"; break;
    }
    if (fixed) { _snprintf_s(out, cap, _TRUNCATE, "%s", fixed); return; }
    if (vk >= 'A' && vk <= 'Z') { _snprintf_s(out, cap, _TRUNCATE, "%c", vk); return; }
    if (vk >= '0' && vk <= '9') { _snprintf_s(out, cap, _TRUNCATE, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        _snprintf_s(out, cap, _TRUNCATE, "F%d", vk - VK_F1 + 1); return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "vk=%d", vk);
}

// L2 Gothic palette — sepia background, gold borders, parchment text.
// Mirrors the L2 in-game menu look (cf. inventory/system menu).
void ApplyL2GothicStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 3.0f;
    s.FrameRounding    = 2.0f;
    s.GrabRounding     = 2.0f;
    s.TabRounding      = 2.0f;
    s.WindowPadding    = ImVec2(12, 10);
    s.ItemSpacing      = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;

    // Palette
    ImVec4 bg          = ImVec4(0x1a/255.f, 0x14/255.f, 0x10/255.f, 0.94f);
    ImVec4 bgFrame     = ImVec4(0x0d/255.f, 0x0a/255.f, 0x08/255.f, 1.00f);
    ImVec4 border      = ImVec4(0x8b/255.f, 0x69/255.f, 0x14/255.f, 1.00f);
    ImVec4 borderDim   = ImVec4(0x5a/255.f, 0x44/255.f, 0x10/255.f, 1.00f);
    ImVec4 text        = ImVec4(0xe8/255.f, 0xd4/255.f, 0xa0/255.f, 1.00f);
    ImVec4 textDim     = ImVec4(0xa8/255.f, 0x90/255.f, 0x60/255.f, 1.00f);
    ImVec4 accent      = ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 1.00f);
    ImVec4 accentBg    = ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 0.20f);
    ImVec4 accentHover = ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 0.35f);
    ImVec4 titleBg     = ImVec4(0x2a/255.f, 0x1f/255.f, 0x15/255.f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]              = bg;
    c[ImGuiCol_ChildBg]               = ImVec4(0,0,0,0);
    c[ImGuiCol_Border]                = border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0,0,0,0);
    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_FrameBg]               = bgFrame;
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0x2a/255.f, 0x1f/255.f, 0x15/255.f, 1.0f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0x3a/255.f, 0x2a/255.f, 0x1c/255.f, 1.0f);
    c[ImGuiCol_TitleBg]               = titleBg;
    c[ImGuiCol_TitleBgActive]         = titleBg;
    c[ImGuiCol_TitleBgCollapsed]      = titleBg;
    c[ImGuiCol_Button]                = accentBg;
    c[ImGuiCol_ButtonHovered]         = accentHover;
    c[ImGuiCol_ButtonActive]          = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accent;
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_Separator]             = borderDim;
    c[ImGuiCol_SeparatorHovered]      = accentHover;
    c[ImGuiCol_SeparatorActive]       = accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0,0,0,0);
    c[ImGuiCol_ResizeGripHovered]     = accentHover;
    c[ImGuiCol_ResizeGripActive]      = accent;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0x8b/255.f, 0x69/255.f, 0x14/255.f, 0.3f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0x8b/255.f, 0x69/255.f, 0x14/255.f, 0.5f);
    c[ImGuiCol_ScrollbarGrabActive]   = accent;
    c[ImGuiCol_Tab]                   = ImVec4(0x2a/255.f, 0x1f/255.f, 0x15/255.f, 1.0f);
    c[ImGuiCol_TabHovered]            = accentHover;
    c[ImGuiCol_TabActive]             = accentBg;
    c[ImGuiCol_TabUnfocused]          = ImVec4(0x1a/255.f, 0x14/255.f, 0x10/255.f, 1.0f);
    c[ImGuiCol_TabUnfocusedActive]    = accentBg;
    c[ImGuiCol_Header]                = accentBg;
    c[ImGuiCol_HeaderHovered]         = accentHover;
    c[ImGuiCol_HeaderActive]          = accent;
}

void Chip(const char* text,
          ImVec4 color = ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f)) {
    ImVec4 bg = color; bg.w = 0.15f;
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bg);
    ImGui::PushStyleColor(ImGuiCol_Text,          color);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
    ImGui::SmallButton(text);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawConnectionDot(bool ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += 8;
    ImU32 col = ok ? IM_COL32(74, 222, 128, 255) : IM_COL32(160, 80, 80, 255);
    dl->AddCircleFilled(ImVec2(p.x + 4, p.y), 4.0f, col);
    if (ok) {
        dl->AddCircleFilled(ImVec2(p.x + 4, p.y), 7.0f,
            IM_COL32(74, 222, 128, 50));
    }
    ImGui::Dummy(ImVec2(12, 0));
    ImGui::SameLine();
}

// =============================================================
// Tab bodies
// =============================================================

void DrawProximityTab(const OverlayState& st) {
    // ----- Master volume -----
    ImGui::TextDisabled("master volume");
    ImGui::SameLine(ImGui::GetWindowWidth() - 60);
    ImGui::Text("%d%%", (int)(st.master_volume * 100));
    float vol = st.master_volume;
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 2.0f, "", ImGuiSliderFlags_NoInput)) {
        SetMasterVolume(vol);
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();

    // ----- Toggles -----
    bool focus = st.require_focus;
    if (ImGui::Checkbox("require window focus", &focus)) {
        SetRequireFocus(focus);
    }
    bool on = st.always_on;
    if (ImGui::Checkbox("always on (no PTT)", &on)) {
        SetAlwaysOn(on);
    }

    ImGui::Spacing();

    // ----- PTT -----
    ImGui::TextUnformatted("push-to-talk");
    ImGui::SameLine();
    bool capturing = g_captureNextKey.load();
    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(255/255.f, 178/255.f, 51/255.f, 1.0f));
        ImGui::TextUnformatted("press a key");
        ImGui::PopStyleColor();
    } else {
        char vkLabel[32];
        VkToString(st.ptt_proximity_vk, vkLabel, sizeof(vkLabel));
        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        Chip(vkLabel);
        ImGui::SameLine();
        if (ImGui::SmallButton("rebind")) {
            g_captureNextKey.store(true);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ----- Speakers + mute-all -----
    ImGui::TextDisabled("speakers");
    ImGui::SameLine();
    ImGui::TextDisabled("(%d active)", st.active_speakers);
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    SpeakerInfo infos[64];
    size_t n = 0;
    GetSpeakerList(infos, 64, n);
    // Detect whether ANY speaker is currently un-muted, to choose
    // between "mute all" and "unmute all".
    bool anyUnmuted = false;
    for (size_t i = 0; i < n; ++i) if (!infos[i].muted) { anyUnmuted = true; break; }
    if (ImGui::SmallButton(anyUnmuted ? "mute all" : "unmute all")) {
        for (size_t i = 0; i < n; ++i) {
            SetSpeakerMuted(infos[i].src_id, anyUnmuted);
        }
    }

    ImGui::BeginChild("##speakers", ImVec2(0, 120), false,
        ImGuiWindowFlags_HorizontalScrollbar);
    if (n == 0) {
        ImGui::TextDisabled("  (no one nearby)");
    }
    for (size_t i = 0; i < n; ++i) {
        ImGui::PushID((int)infos[i].src_id);
        bool m = infos[i].muted;
        if (ImGui::Checkbox("##mute", &m)) {
            SetSpeakerMuted(infos[i].src_id, m);
        }
        ImGui::SameLine();
        bool speaking = infos[i].ms_since_mix < 200;
        ImVec4 col = speaking ? ImVec4(74/255.f, 222/255.f, 128/255.f, 1.0f)
                              : ImVec4(232/255.f, 234/255.f, 240/255.f, 1.0f);
        char name[48];
        bool haveName = GetSpeakerName(infos[i].src_id, name, sizeof(name));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (haveName && name[0]) {
            ImGui::Text("%s", name);
        } else {
            ImGui::Text("sid=%u", infos[i].src_id);
        }
        ImGui::PopStyleColor();
        if (speaking) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(74/255.f, 222/255.f, 128/255.f, 1.0f));
            ImGui::TextUnformatted("●");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void DrawComingSoon(const char* channelName) {
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(139/255.f, 146/255.f, 163/255.f, 1.0f));
    ImGui::TextWrapped(
        "%s channel not yet implemented.\n\n"
        "Phase 5 will add group voice (members of your %s only, no "
        "spatial attenuation). For now use proximity.", channelName, channelName);
    ImGui::PopStyleColor();
}

// =============================================================
// Main panel
// =============================================================

// Returns true if a frame should be drawn this tick. When false, the
// caller MUST skip ImGui_ImplWin32_NewFrame entirely — that backend
// otherwise calls SetCursor every frame, fighting the L2 game's own
// cursor management and flickering badly. (Pattern lifted from the
// existing l2ui DLL where we already hit and fixed this same bug.)
bool ShouldDrawFrame() {
    if (!g_visible.load(std::memory_order_relaxed)) return false;
    OverlayState st = SnapshotOverlayState();
    return st.session_id != 0;
}

// Small square icon (48x48) shown when the panel is minimized.
// Visually: dark sepia background, gold border, "VOX" centered in
// gold. Drag from anywhere on the icon. Double-click to restore.
void DrawMinimized() {
    ImGui::SetNextWindowSize(ImVec2(56, 56), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##l2voice_min", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 p1 = ImVec2(p0.x + 56, p0.y + 56);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Sepia bg + double gold border (matches design 03 L2 Gothic feel)
    dl->AddRectFilled(p0, p1,
        IM_COL32(0x1a, 0x14, 0x10, 0xee), 4.0f);
    dl->AddRect(p0, p1,
        IM_COL32(0xd4, 0xaf, 0x37, 0xff), 4.0f, 0, 2.0f);
    dl->AddRect(ImVec2(p0.x + 3, p0.y + 3), ImVec2(p1.x - 3, p1.y - 3),
        IM_COL32(0x5a, 0x44, 0x10, 0xff), 2.0f, 0, 1.0f);

    // Invisible button covering the whole icon for hit-testing.
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##icon_hit", ImVec2(56, 56));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    // Drag to move (when held + mouse delta).
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos(ImVec2(p0.x + d.x, p0.y + d.y));
    }
    // Double-click anywhere on icon → restore.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        g_minimized.store(false);
    }

    // Centered microphone glyph: PNG if we managed to load it,
    // otherwise fall back to "VOX" text in gold.
    if (g_micTexture) {
        const float pad = 8.0f;
        ImVec2 imgP0(p0.x + pad, p0.y + pad);
        ImVec2 imgP1(p1.x - pad, p1.y - pad);
        // Tint: dim gold normally, brighter on hover.
        ImU32 tint = hovered
            ? IM_COL32(0xff, 0xd6, 0x60, 0xff)
            : IM_COL32(0xd4, 0xaf, 0x37, 0xff);
        dl->AddImage(reinterpret_cast<ImTextureID>(g_micTexture),
            imgP0, imgP1, ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
        const char* label = "VOX";
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImVec2 tp = ImVec2(p0.x + (56 - ts.x) * 0.5f, p0.y + (56 - ts.y) * 0.5f);
        ImU32 col = hovered ? IM_COL32(0xff, 0xd6, 0x60, 0xff)
                            : IM_COL32(0xd4, 0xaf, 0x37, 0xff);
        dl->AddText(tp, col, label);
    }
    ImGui::End();
}

void DrawPanel() {
    if (g_minimized.load()) {
        DrawMinimized();
        return;
    }

    OverlayState st = SnapshotOverlayState();

    // Real ImGui window — title bar is back so the user can drag it
    // around. Built-in collapse is disabled (NoCollapse): we use our
    // own minimize button that goes to a separate icon window.
    ImGui::SetNextWindowSize(ImVec2(320, 430), ImGuiCond_FirstUseEver);
    char titleBuf[64];
    _snprintf_s(titleBuf, sizeof(titleBuf), _TRUNCATE,
        "l2voice  %s###l2voice_window",
        st.ws_connected ? "[connected]" : "[offline]");
    if (!ImGui::Begin(titleBuf, nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Custom minimize button — right-aligned on its own row before the
    // tabs. Click → render as a 56x56 icon next frame. Use "_" since
    // the default ImGui font ships only ASCII (em-dash renders as "?").
    float btnW = ImGui::CalcTextSize("_").x + 16.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btnW - 6.0f);
    if (ImGui::SmallButton(" _ ##min")) {
        g_minimized.store(true);
    }

    // ====== Session + player name (header info area) ======
    // For "player" we show the CHARACTER NAME, queried via the same
    // sid → name machinery used for other speakers. Looking up our
    // own session id resolves to our own player on the server side.
    ImGui::TextDisabled("session");
    ImGui::SameLine(ImGui::GetWindowWidth() - 72);
    char sidLabel[24];
    _snprintf_s(sidLabel, sizeof(sidLabel), _TRUNCATE, "sid %u", st.session_id);
    Chip(sidLabel);

    ImGui::TextDisabled("player");
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    char myName[48];
    bool haveMyName = GetSpeakerName(st.session_id, myName, sizeof(myName));
    if (haveMyName && myName[0]) {
        Chip(myName);
    } else if (st.player_id != 0) {
        char pid[16];
        _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%u", st.player_id);
        Chip(pid);
    } else {
        ImGui::TextDisabled("?");
    }
    ImGui::Separator();

    // ====== Tabs ======
    if (ImGui::BeginTabBar("##chs")) {
        if (ImGui::BeginTabItem("Proximity")) {
            DrawProximityTab(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Party")) {
            DrawComingSoon("Party");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Clan")) {
            DrawComingSoon("Clan");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Ally")) {
            DrawComingSoon("Ally");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Insert hides  ·  _ minimizes  ·  drag titlebar to move");
    ImGui::End();
}

// =============================================================
// WndProc
// =============================================================

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (int)wp == g_toggleVk) {
        bool prev = g_visible.exchange(!g_visible.load());
        Logf("[l2voice] overlay toggled %s\n", prev ? "OFF" : "ON");
        return 0;
    }
    if (g_captureNextKey.load()) {
        int capturedVk = 0;
        bool cancel = false;
        if (msg == WM_KEYDOWN) {
            int vk = (int)wp;
            if (vk == VK_ESCAPE) cancel = true;
            else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                     vk != VK_LSHIFT && vk != VK_RSHIFT &&
                     vk != VK_LCONTROL && vk != VK_RCONTROL &&
                     vk != VK_LMENU && vk != VK_RMENU) capturedVk = vk;
        } else if (msg == WM_RBUTTONDOWN) capturedVk = VK_RBUTTON;
        else if (msg == WM_MBUTTONDOWN)  capturedVk = VK_MBUTTON;
        else if (msg == WM_XBUTTONDOWN)
            capturedVk = (HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        if (cancel) { g_captureNextKey.store(false); return 0; }
        if (capturedVk != 0) {
            g_captureNextKey.store(false);
            SetPttProximityVk(capturedVk);
            Logf("[l2voice] PTT rebound to vk=%d\n", capturedVk);
            return 0;
        }
    }

    if (g_imguiBackendInit.load()) {
        ImGui::SetCurrentContext(g_imguiCtx);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();

        // Pattern from l2ui: when ImGui wants the mouse, return 1 to
        // WM_SETCURSOR (NOT setting it ourselves). The cursor stays
        // whatever the previous WndProc set — no fight.
        if (io.WantCaptureMouse && msg == WM_SETCURSOR) {
            return 1;
        }

        bool isMouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                       msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
        bool isKey   = (msg == WM_KEYDOWN || msg == WM_KEYUP ||
                        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                        msg == WM_CHAR);
        if (isMouse && io.WantCaptureMouse)    return 0;
        if (isKey   && io.WantCaptureKeyboard) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// =============================================================
// D3D9 hooks
// =============================================================

HRESULT WINAPI HookEndScene(IDirect3DDevice9* dev) {
    if (!g_imguiBackendInit.load()) {
        ImGui::SetCurrentContext(g_imguiCtx);

        IDirect3DSwapChain9* swap = nullptr;
        HWND hwnd = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &swap)) && swap) {
            D3DPRESENT_PARAMETERS pp = {};
            swap->GetPresentParameters(&pp);
            hwnd = pp.hDeviceWindow;
            swap->Release();
        }
        if (!hwnd) hwnd = GetForegroundWindow();
        g_targetHwnd = hwnd;

        Logf("[l2voice] overlay: initializing on hwnd=%p\n", hwnd);
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX9_Init(dev);

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // CRITICAL: ConfigFlags (not BackendFlags) is the right knob.
        // l2ui hit the same cursor-fight bug — see comments in their
        // d3d9_hook.cpp around line 1303. NoMouseCursorChange tells
        // the Win32 backend's NewFrame to never call ::SetCursor.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

        ApplyL2GothicStyle();

        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(&HookedWndProc)));

        // Load the microphone icon for the minimized state. Lives next
        // to l2voice.dll. Falls back to text if missing.
        wchar_t iconPath[MAX_PATH];
        ResolveDllRelativePath(L"voice-recorder.png", iconPath, MAX_PATH);
        g_micTexture = LoadPngAsTexture(dev, iconPath, g_micW, g_micH);
        if (g_micTexture) {
            Logf("[l2voice] icon loaded: %dx%d\n", g_micW, g_micH);
        } else {
            Logf("[l2voice] icon NOT loaded (path=%ws) — fallback to text\n",
                iconPath);
        }

        g_imguiBackendInit.store(true);
    }

    ImGui::SetCurrentContext(g_imguiCtx);
    // CRITICAL: skip the frame entirely when the panel won't draw.
    // ImGui_ImplWin32_NewFrame calls SetCursor every frame regardless
    // of whether anything renders — calling it while the panel is
    // hidden makes the cursor flicker between L2's custom cursor and
    // the OS arrow.
    if (ShouldDrawFrame()) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawPanel();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        // Sample WantCaptureMouse for the GetAsyncKeyState hook so it
        // knows whether to mask clicks from the game's input polling.
        g_imguiCapturesMouse.store(
            ImGui::GetIO().WantCaptureMouse, std::memory_order_relaxed);
    } else {
        g_imguiCapturesMouse.store(false, std::memory_order_relaxed);
    }

    return g_origEndScene(dev);
}

HRESULT WINAPI HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_imguiBackendInit.load()) {
        ImGui::SetCurrentContext(g_imguiCtx);
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    HRESULT hr = g_origReset(dev, pp);
    if (SUCCEEDED(hr) && g_imguiBackendInit.load()) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return hr;
}

bool GetDeviceVTableEntries(void*& endSceneOut, void*& resetOut) {
    using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) return false;
    auto pCreate = reinterpret_cast<PFN_Direct3DCreate9>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!pCreate) return false;
    IDirect3D9* d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) return false;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow    = GetDesktopWindow();

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, GetDesktopWindow(),
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) { d3d->Release(); return false; }
    void** vt = *reinterpret_cast<void***>(dev);
    resetOut    = vt[16];
    endSceneOut = vt[42];
    dev->Release();
    d3d->Release();
    return true;
}

}  // namespace

bool InstallOverlay() {
    if (g_imguiCtx) return true;
    void* endSceneAddr = nullptr;
    void* resetAddr    = nullptr;
    if (!GetDeviceVTableEntries(endSceneAddr, resetAddr)) {
        Logf("[l2voice] overlay: GetDeviceVTableEntries failed\n");
        return false;
    }
    Logf("[l2voice] overlay: EndScene=%p Reset=%p\n", endSceneAddr, resetAddr);

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("[l2voice] overlay: MH_Initialize failed: %d\n", s);
        return false;
    }
    // Also hook GetAsyncKeyState in user32 so the game's polling
    // input loop (typical for L2: GetAsyncKeyState(VK_LBUTTON) every
    // tick to detect clicks) reports "no click" while the panel has
    // the mouse. Pure WndProc consume isn't enough — the kernel
    // still tracks the physical button state, and GetAsyncKeyState
    // reads from there, bypassing message processing.
    HMODULE user32 = GetModuleHandleA("user32.dll");
    void* gaksAddr = user32 ? GetProcAddress(user32, "GetAsyncKeyState") : nullptr;

    if (MH_CreateHook(endSceneAddr,
            reinterpret_cast<void*>(&HookEndScene),
            reinterpret_cast<void**>(&g_origEndScene)) != MH_OK ||
        MH_CreateHook(resetAddr,
            reinterpret_cast<void*>(&HookReset),
            reinterpret_cast<void**>(&g_origReset)) != MH_OK) {
        Logf("[l2voice] overlay: D3D9 hook install failed\n");
        return false;
    }
    if (gaksAddr) {
        if (MH_CreateHook(gaksAddr,
                reinterpret_cast<void*>(&HookGetAsyncKeyState),
                reinterpret_cast<void**>(&g_origGetAsyncKeyState)) != MH_OK) {
            Logf("[l2voice] overlay: GetAsyncKeyState hook install failed\n");
        }
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("[l2voice] overlay: MH_EnableHook(ALL) failed\n");
        return false;
    }

    // DirectInput8 mouse-button filter (additional hook layer beyond
    // WndProc + GetAsyncKeyState). DI lives in dinput8.dll and L2
    // creates its mouse device there; we late-bind via vtable.
    InstallDirectInputHook();

    IMGUI_CHECKVERSION();
    g_imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imguiCtx);

    Logf("[l2voice] overlay: hooks armed, waiting for first EndScene\n");
    return true;
}

void UninstallOverlay() {
    if (g_imguiBackendInit.load()) {
        ImGui::SetCurrentContext(g_imguiCtx);
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }
    if (g_targetHwnd && g_origWndProc) {
        SetWindowLongPtrW(g_targetHwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_origWndProc));
    }
    MH_DisableHook(MH_ALL_HOOKS);
    if (g_imguiCtx) {
        ImGui::DestroyContext(g_imguiCtx);
        g_imguiCtx = nullptr;
    }
    g_imguiBackendInit.store(false);
}

}  // namespace voice
