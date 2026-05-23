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
#include "resources.h"
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
#include <chrono>
#include <climits>
#include <cmath>
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
// Per-state mic icons used in the minimized speaker overlay.
// Loaded once at backend init. Pre-tinted=0 so the original PNG
// colors (green/red) are preserved.
IDirect3DTexture9* g_micSpeakingTex = nullptr;
IDirect3DTexture9* g_micMutedTex    = nullptr;
IDirect3DTexture9* g_micBlockedTex  = nullptr;

// L2UI_CH3 native button textures (extracted from the L2 Essence
// client). Used by the L2Button / L2SmallButton helpers below.
IDirect3DTexture9* g_l2BtnBig         = nullptr;
IDirect3DTexture9* g_l2BtnBigOver     = nullptr;
IDirect3DTexture9* g_l2BtnBigDown     = nullptr;
IDirect3DTexture9* g_l2BtnSmall       = nullptr;
IDirect3DTexture9* g_l2BtnSmallOver   = nullptr;
IDirect3DTexture9* g_l2BtnSmallDown   = nullptr;
IDirect3DTexture9* g_l2FrameMini      = nullptr;
IDirect3DTexture9* g_l2FrameMiniOver  = nullptr;
IDirect3DTexture9* g_l2FrameMiniDown  = nullptr;
IDirect3DTexture9* g_l2FrameClose     = nullptr;
IDirect3DTexture9* g_l2FrameCloseOver = nullptr;
IDirect3DTexture9* g_l2FrameCloseDown = nullptr;
// L2 tab textures (HennaWnd_TabBtn series).
IDirect3DTexture9* g_l2TabSelected         = nullptr;
IDirect3DTexture9* g_l2TabUnselected       = nullptr;
IDirect3DTexture9* g_l2TabUnselectedOver   = nullptr;
// L2 native window backdrop (NpcWnd.npc1_back) — used as the main
// panel background instead of the procedural sepia rect.
IDirect3DTexture9* g_l2WndBg               = nullptr;
ImGuiContext*   g_imguiCtx     = nullptr;
std::atomic<bool> g_imguiBackendInit{false};
std::atomic<bool> g_visible{true};
std::atomic<bool> g_minimized{false};
std::atomic<bool> g_imguiCapturesMouse{false};  // sampled each frame; read by input hooks
int             g_toggleVk      = VK_INSERT;
std::atomic<bool> g_captureNextKey{false};
// Which PTT slot the next key capture binds to: 0=Proximity, 1=Party,
// 2=Clan, 3=Ally, 4=CC. Set by the rebind button on each tab. Used by
// the WndProc capture handler.
std::atomic<int>  g_captureNextSlot{0};

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
    void** diVT = *reinterpret_cast<void***>(di);
    void* createDevAddr = diVT[3];   // IDirectInput8::CreateDevice
    Logf("[l2voice] hooking IDirectInput8::CreateDevice=%p\n", createDevAddr);
    if (MH_CreateHook(createDevAddr,
            reinterpret_cast<void*>(&HookDICreateDevice),
            reinterpret_cast<void**>(&g_origCreateDevice)) == MH_OK) {
        MH_EnableHook(createDevAddr);
    }

    // L2 typically creates its mouse device during startup — BEFORE
    // our CreateDevice hook is in place. To cover that case, create
    // our own temporary mouse device through `di` here and hook the
    // GetDeviceState/GetDeviceData entries on its vtable. Because
    // IDirectInputDevice8's vtable is class-level (one table shared
    // by all instances), hooking through our device patches L2's
    // already-existing device too.
    IDirectInputDevice8A* tempMouse = nullptr;
    HRESULT hrm = di->CreateDevice(GUID_SysMouse, &tempMouse, nullptr);
    if (SUCCEEDED(hrm) && tempMouse && !g_diMouseHooked.exchange(true)) {
        void** vt = *reinterpret_cast<void***>(tempMouse);
        void* gs = vt[9];   // IDirectInputDevice8::GetDeviceState
        void* gd = vt[10];  // IDirectInputDevice8::GetDeviceData
        Logf("[l2voice] hooking IDirectInputDevice8 vt: GetDeviceState=%p GetDeviceData=%p (early)\n",
             gs, gd);
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
        tempMouse->Release();
    } else if (FAILED(hrm)) {
        Logf("[l2voice] temp mouse create failed: %08lx — falling back to hook-on-CreateDevice\n",
             hrm);
    }
    di->Release();
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

// Decodes PNG bytes (in memory) via stb_image, OPTIONALLY pre-tints
// all non-transparent pixels to a solid color, then uploads to a
// managed D3D9 texture. Returns the texture pointer (and out w/h)
// on success; nullptr on any failure.
//
// tintRgb: 0xRRGGBB to recolor opaque pixels; pass 0 to leave the
// original colors alone.
IDirect3DTexture9* LoadPngBufferAsTexture(IDirect3DDevice9* dev,
        const void* buffer, size_t bufferSize,
        int& w, int& h, uint32_t tintRgb = 0) {
    int c = 0;
    unsigned char* px = stbi_load_from_memory(
        static_cast<const unsigned char*>(buffer), (int)bufferSize,
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
    // If tint is non-zero, replace RGB with the tint color while
    // preserving the alpha (= use the original image as an opacity
    // mask). Anti-aliased pixels (alpha 1..254) blend smoothly.
    const unsigned char tintR = (unsigned char)((tintRgb >> 16) & 0xFF);
    const unsigned char tintG = (unsigned char)((tintRgb >>  8) & 0xFF);
    const unsigned char tintB = (unsigned char)( tintRgb        & 0xFF);
    for (int y = 0; y < h; ++y) {
        unsigned char* src = px + y * w * 4;
        unsigned char* dst = (unsigned char*)lr.pBits + y * lr.Pitch;
        for (int x = 0; x < w; ++x) {
            unsigned char a = src[x*4 + 3];
            if (tintRgb != 0) {
                dst[x*4 + 0] = tintB;
                dst[x*4 + 1] = tintG;
                dst[x*4 + 2] = tintR;
            } else {
                dst[x*4 + 0] = src[x*4 + 2];   // B
                dst[x*4 + 1] = src[x*4 + 1];   // G
                dst[x*4 + 2] = src[x*4 + 0];   // R
            }
            dst[x*4 + 3] = a;
        }
    }
    tex->UnlockRect(0);
    stbi_image_free(px);
    return tex;
}

// Loads a PNG embedded as RCDATA in this DLL into a D3D9 texture.
// `resId` is one of the IDR_* values from resources.h.
// `tintRgb=0` preserves the original PNG colors (used for the three
// state-coloured mic icons). Non-zero replaces RGB with the tint
// keeping alpha as a mask (used for the toolbar mic).
IDirect3DTexture9* LoadEmbeddedPng(IDirect3DDevice9* dev,
        int resId, int& w, int& h, uint32_t tintRgb) {
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadPngBufferAsTexture), &self);
    HRSRC hRes = FindResourceW(self, MAKEINTRESOURCEW(resId),
        reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!hRes) return nullptr;
    HGLOBAL hData = LoadResource(self, hRes);
    if (!hData) return nullptr;
    void* bytes = LockResource(hData);
    DWORD size = SizeofResource(self, hRes);
    if (!bytes || size == 0) return nullptr;
    return LoadPngBufferAsTexture(dev, bytes, size, w, h, tintRgb);
}

// Back-compat alias for the toolbar mic loader.
IDirect3DTexture9* LoadEmbeddedMicTexture(IDirect3DDevice9* dev,
        int& w, int& h, uint32_t tintRgb) {
    return LoadEmbeddedPng(dev, IDR_MIC_PNG, w, h, tintRgb);
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
// L2-native button widgets
// =============================================================
//
// Render a 3-state button using L2UI_CH3 textures (extracted from the
// Essence client). The stock ImGui::Button isn't quite L2-native — the
// real buttons use a gold-bordered dark navy panel with subtle hover/
// pressed states. We replicate that with InvisibleButton hit-testing
// and our own drawlist composite: texture background + centered text.
//
// Falls back to a procedural gold-on-sepia rect if textures didn't
// load (e.g. resource not found on a stripped build).

namespace {
bool L2ButtonImpl(const char* label, ImVec2 size,
                  IDirect3DTexture9* tn, IDirect3DTexture9* th,
                  IDirect3DTexture9* ta) {
    if (size.x <= 0) {
        ImVec2 textSz = ImGui::CalcTextSize(label);
        size.x = textSz.x + 32.0f;
    }
    if (size.y <= 0) size.y = 24.0f;

    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##l2btn", size);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();

    IDirect3DTexture9* tex = tn;
    if (active && ta)       tex = ta;
    else if (hovered && th) tex = th;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex) {
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), p0, p1);
    } else {
        // Procedural fallback: dark navy + gold border.
        ImU32 bg     = active   ? IM_COL32(0x18, 0x22, 0x40, 0xff)
                     : hovered  ? IM_COL32(0x24, 0x32, 0x58, 0xff)
                                : IM_COL32(0x10, 0x18, 0x30, 0xff);
        ImU32 border = IM_COL32(0xd4, 0xaf, 0x37, 0xff);
        dl->AddRectFilled(p0, p1, bg, 2.0f);
        dl->AddRect(p0, p1, border, 2.0f, 0, 1.5f);
    }

    // Centered text — soft black shadow + parchment gold.
    ImVec2 textSz = ImGui::CalcTextSize(label);
    ImVec2 tp(p0.x + (size.x - textSz.x) * 0.5f,
              p0.y + (size.y - textSz.y) * 0.5f);
    ImU32 textCol = hovered ? IM_COL32(0xff, 0xe8, 0xa8, 0xff)
                            : IM_COL32(0xe8, 0xd4, 0xa0, 0xff);
    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label);
    dl->AddText(tp, textCol, label);

    ImGui::PopID();
    return clicked;
}
} // namespace

// L2Button — full-width "BigButton3" style. Use for prominent
// actions (clan modes, channel selectors).
bool L2Button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    return L2ButtonImpl(label, size, g_l2BtnBig, g_l2BtnBigOver, g_l2BtnBigDown);
}

// L2SmallButton — compact "SmallButton1" style. Use for row-local
// actions (rebind, RM, grant, etc.).
bool L2SmallButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    return L2ButtonImpl(label, size,
                        g_l2BtnSmall, g_l2BtnSmallOver, g_l2BtnSmallDown);
}

// L2TabBar — custom horizontal tab strip that mimics the L2 native UI
// (HennaWnd_TabBtn series). Each tab is a clickable invisible button
// over a stretched texture; selected tab uses the dark-brown
// "Selected" texture, others use the gray "Unselected" (lighter on
// hover). After this call the ImGui cursor is positioned just below
// the strip so the caller can render the active tab's body.
//
// `visible` is optional — pass nullptr to show every label, or an
// array of `count` bools to gate individual tabs (used by the CC tab
// which only appears when in a command channel).
//
// `activeOut` is in/out: starts as the currently-active index, and
// is updated to the clicked tab. Returns the active index for
// convenience.
int L2TabBar(const char* id, const char* const* labels, int count,
             int* activeOut, const bool* visible = nullptr) {
    ImGui::PushID(id);
    int active = activeOut ? *activeOut : 0;
    if (active < 0 || active >= count) active = 0;

    int visibleCount = 0;
    for (int i = 0; i < count; ++i) {
        if (!visible || visible[i]) ++visibleCount;
    }
    if (visibleCount == 0) {
        ImGui::PopID();
        return 0;
    }

    const float totalW = ImGui::GetContentRegionAvail().x;
    const float tabW   = totalW / (float)visibleCount;
    const float tabH   = 26.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();

    int slot = 0;
    for (int i = 0; i < count; ++i) {
        if (visible && !visible[i]) continue;
        ImVec2 p0(start.x + slot * tabW, start.y);
        ImVec2 p1(p0.x + tabW, p0.y + tabH);

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(p0);
        bool clicked = ImGui::InvisibleButton("##tab", ImVec2(tabW, tabH));
        bool hovered = ImGui::IsItemHovered();
        if (clicked && activeOut) {
            active = i;
            *activeOut = i;
        }

        IDirect3DTexture9* tex;
        if (i == active)   tex = g_l2TabSelected;
        else if (hovered)  tex = g_l2TabUnselectedOver;
        else               tex = g_l2TabUnselected;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (tex) {
            dl->AddImage(reinterpret_cast<ImTextureID>(tex), p0, p1);
        } else {
            // Procedural fallback: dark brown (selected) / gray (idle).
            ImU32 bg = (i == active)
                ? IM_COL32(0x2a, 0x1f, 0x15, 0xff)
                : hovered ? IM_COL32(0x70, 0x68, 0x60, 0xff)
                          : IM_COL32(0x55, 0x50, 0x48, 0xff);
            dl->AddRectFilled(p0, p1, bg, 2.0f);
            dl->AddRect(p0, p1, IM_COL32(0xd4, 0xaf, 0x37, 0xff), 2.0f);
        }

        ImVec2 textSz = ImGui::CalcTextSize(labels[i]);
        ImVec2 tp(p0.x + (tabW - textSz.x) * 0.5f,
                  p0.y + (tabH - textSz.y) * 0.5f);
        ImU32 textCol = (i == active)
            ? IM_COL32(0xff, 0xe8, 0xa8, 0xff)
            : hovered ? IM_COL32(0xff, 0xf0, 0xc0, 0xff)
                      : IM_COL32(0x40, 0x38, 0x30, 0xff);
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 180), labels[i]);
        dl->AddText(tp, textCol, labels[i]);

        ImGui::PopID();
        ++slot;
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + tabH + 4));
    ImGui::PopID();
    return active;
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

    // Transmit-here selector — mirrors the radio on group tabs. The
    // Proximity tab is the default; toggling off here would put TX
    // in a no-channel state, so we don't allow that — clicking just
    // re-asserts proximity.
    int activeTx = GetActiveTxChannel();
    bool txHere = (activeTx == 0);
    if (ImGui::Checkbox("transmit here (PTT)", &txHere)) {
        SetActiveTxChannel(0);
    }
    if (txHere) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "active");
    }

    ImGui::Spacing();

    // ----- PTT -----
    ImGui::TextUnformatted("push-to-talk");
    ImGui::SameLine();
    bool capturing = g_captureNextKey.load() && g_captureNextSlot.load() == 0;
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
            g_captureNextSlot.store(0);
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

        // Per-player volume slider — compact, right-anchored. Drag to
        // boost/attenuate this single speaker. Default 1.0.
        const float sliderW = 80.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - sliderW - 12.0f);
        float v = infos[i].volume;
        ImGui::PushItemWidth(sliderW);
        if (ImGui::SliderFloat("##vol", &v, 0.0f, 2.0f, "",
                ImGuiSliderFlags_NoInput)) {
            SetSpeakerVolume(infos[i].src_id, v);
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Helper — render one row of the per-tab member list.
//   - Name (resolved via player_id lookup, falls back to pid string)
//   - voice_active dimmer when the member has no live voice session
//   - Mute checkbox (uses set_player_mute) — sticky across tabs
//   - Per-player volume slider (uses set_player_volume)
// Optional `extra_glyph` is appended in front of the name (e.g. "★"
// for sub-leaders, "👑" for leader). Pass nullptr to omit.
//
// `channelId` is for visual scoping only — mute/volume state is global
// per-player on the server side.
// `channelId` is the wire-protocol channel of the tab this row is in
// (1=Party, 2=Clan, 3=Ally, 4=CC). Drives leader-only controls:
// the remote-mute button only appears on Clan/Ally when the local
// player is leader or sub-leader.
void DrawMemberRow(const OverlayMember& m, const char* extra_glyph,
                   int channelId) {
    ImGui::PushID((int)m.player_id);
    bool dimmed = !m.voice_active;
    if (dimmed) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(110/255.f, 116/255.f, 130/255.f, 1.0f));
    }

    char name[64];
    bool haveName = GetPlayerName(m.player_id, name, sizeof(name));

    // Local mute checkbox — affects only what THIS client hears.
    bool muted = false;
    if (ImGui::Checkbox("##mute", &muted)) {
        SetSpeakerMuted(m.player_id, true);
        SetSpeakerVolume(m.player_id, 0.0f);
    }
    ImGui::SameLine();

    if (extra_glyph) {
        ImGui::TextUnformatted(extra_glyph);
        ImGui::SameLine();
    }
    if (haveName && name[0]) ImGui::Text("%s", name);
    else                     ImGui::Text("pid=%u", m.player_id);

    if (dimmed) {
        ImGui::SameLine();
        ImGui::TextDisabled("(offline)");
    }

    // Right-anchored controls: [RM] (clan/ally for leaders) + volume slider.
    const float sliderW = 80.0f;
    const float rmBtnW  = 28.0f;
    bool canRemoteMute = (channelId == 2 || channelId == 3)
                       && GetLocalRole() >= 1
                       && m.player_id != 0
                       && m.player_id != /*self never*/ 0xFFFFFFFFu;
    // Self-target: hide button on our own row.
    {
        OverlayState st = SnapshotOverlayState();
        if (m.player_id == st.player_id) canRemoteMute = false;
    }
    float rightX = ImGui::GetWindowWidth() - sliderW - 12.0f;
    if (canRemoteMute) rightX -= rmBtnW + 6.0f;
    ImGui::SameLine(rightX);
    if (canRemoteMute) {
        // Red when this member is currently remote-muted, dim gray otherwise.
        ImVec4 btnCol = m.remote_muted
            ? ImVec4(0.78f, 0.20f, 0.20f, 1.0f)
            : ImVec4(0.35f, 0.35f, 0.38f, 1.0f);
        ImVec4 btnHover = m.remote_muted
            ? ImVec4(0.88f, 0.28f, 0.28f, 1.0f)
            : ImVec4(0.45f, 0.45f, 0.50f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        btnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnCol);
        if (ImGui::Button("RM", ImVec2(rmBtnW, 0))) {
            const char* scope = (channelId == 2) ? "clan" : "ally";
            SendClanRemoteMute(m.player_id, !m.remote_muted, scope);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(m.remote_muted
                ? "Remote-muted (click to unmute)"
                : "Remote mute on %s",
                (channelId == 2) ? "clan" : "ally");
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    }

    // Per-player local volume slider, far right.
    float v = 1.0f;
    ImGui::PushItemWidth(sliderW);
    if (ImGui::SliderFloat("##vol", &v, 0.0f, 2.0f, "",
            ImGuiSliderFlags_NoInput)) {
        SetSpeakerVolume(m.player_id, v);
    }
    ImGui::PopItemWidth();

    if (dimmed) ImGui::PopStyleColor();
    ImGui::PopID();
}

// Common body for the Party / Clan / Ally tabs. Renders:
//   - enable toggle for incoming audio on this channel
//   - channel volume slider
//   - member list (server-pushed via client_state)
//
// `channelId` matches the wire protocol: 1=Party, 2=Clan, 3=Ally, 4=CC.
void DrawGroupTab(const OverlayState& st, int channelId, const char* label) {
    // channelId may be 4 (CC) — for prefs we reuse the CLAN slot since
    // we only have 4 slots in voice.ini. Doesn't matter for routing.
    int prefSlot = (channelId == 4) ? 2 : channelId;

    bool enabled = st.ch_enabled[prefSlot];
    if (ImGui::Checkbox("hear this channel", &enabled)) {
        SetChannelEnabled(prefSlot, enabled);
    }

    // Transmit selector — radio across all tabs (mutually exclusive).
    int activeTx = GetActiveTxChannel();
    bool txHere = (activeTx == channelId);
    if (ImGui::Checkbox("transmit here (PTT)", &txHere)) {
        // Toggle on -> set this channel as active TX; toggle off ->
        // revert to Proximity (the always-safe default).
        SetActiveTxChannel(txHere ? channelId : 0);
    }
    if (txHere) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "active");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("channel volume");
    ImGui::SameLine(ImGui::GetWindowWidth() - 60);
    ImGui::Text("%d%%", (int)(st.ch_volume[prefSlot] * 100));
    float vol = st.ch_volume[prefSlot];
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##chvol", &vol, 0.0f, 2.0f, "", ImGuiSliderFlags_NoInput)) {
        SetChannelVolume(prefSlot, vol);
    }
    ImGui::PopItemWidth();

    // Optional PTT key for this channel — when held, the capture path
    // routes that frame to this channel regardless of active_tx_channel.
    // (Useful for "push to talk in clan" while keeping default TX on
    // proximity.) The CC tab has no dedicated PTT slot today — it
    // shares party's binding for simplicity (player can rebind via the
    // tab's checkbox; CC is leader-gated anyway).
    int pttVk = 0;
    int slot = 0;
    if (channelId == 1)      { pttVk = GetPttPartyVk(); slot = 1; }
    else if (channelId == 2) { pttVk = GetPttClanVk();  slot = 2; }
    else if (channelId == 3) { pttVk = GetPttAllyVk();  slot = 3; }
    if (channelId >= 1 && channelId <= 3) {
        ImGui::Spacing();
        ImGui::TextUnformatted("push-to-talk");
        ImGui::SameLine();
        bool capturing = g_captureNextKey.load() && g_captureNextSlot.load() == slot;
        if (capturing) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(255/255.f, 178/255.f, 51/255.f, 1.0f));
            ImGui::TextUnformatted("press a key");
            ImGui::PopStyleColor();
        } else {
            char vkLabel[32];
            if (pttVk == 0) _snprintf_s(vkLabel, sizeof(vkLabel), _TRUNCATE, "none");
            else            VkToString(pttVk, vkLabel, sizeof(vkLabel));
            ImGui::SameLine(ImGui::GetWindowWidth() - 130);
            Chip(vkLabel);
            ImGui::SameLine();
            if (ImGui::SmallButton("rebind")) {
                g_captureNextSlot.store(slot);
                g_captureNextKey.store(true);
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    OverlayMember roster[64];
    size_t n = GetGroupRoster((uint8_t)channelId, roster, 64);

    // Count voice-active members so the user sees who actually shows up.
    int activeCount = 0;
    for (size_t i = 0; i < n; ++i) if (roster[i].voice_active) ++activeCount;

    ImGui::TextDisabled("%s members (%d voice-active / %zu total)",
        label, activeCount, n);

    ImGui::BeginChild("##members", ImVec2(0, 160), false,
        ImGuiWindowFlags_HorizontalScrollbar);
    if (n == 0) {
        ImGui::TextDisabled("  (waiting for server roster — auth + L2J events)");
    }

    // Two passes: first leaders/sub-leaders / CC speakers, then plain
    // members. Order makes the leader panel visually distinct.
    auto draw_priority = [&](bool first_priority) {
        for (size_t i = 0; i < n; ++i) {
            const OverlayMember& m = roster[i];
            bool priority = false;
            const char* glyph = nullptr;
            if (channelId == 2) {
                // Clan tab — leaders + subs on top.
                priority = (m.clan_role == 2 || m.clan_role == 1);
                glyph    = (m.clan_role == 2) ? "L" :
                           (m.clan_role == 1) ? "S" : nullptr;
            } else if (channelId == 4) {
                // CC tab — leader + permitted speakers on top.
                priority = m.cc_can_speak;
                uint32_t leaderId = GetCCLeaderID();
                glyph    = (m.player_id == leaderId) ? "L" :
                           m.cc_can_speak           ? "*" : nullptr;
            }
            if (priority == first_priority) {
                DrawMemberRow(m, glyph, channelId);
            }
        }
    };
    if (channelId == 2 || channelId == 4) {
        draw_priority(true);   // leaders/sub-leaders or CC speakers
        if (n > 0) ImGui::Separator();
        draw_priority(false);  // plain members
    } else {
        for (size_t i = 0; i < n; ++i) DrawMemberRow(roster[i], nullptr, channelId);
    }
    ImGui::EndChild();
}

void DrawComingSoon(const char* channelName) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(139/255.f, 146/255.f, 163/255.f, 1.0f));
    ImGui::TextWrapped("%s not yet implemented.", channelName);
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
    // Restore the saved icon position. Use FirstUseEver so we don't
    // fight the user mid-drag — we only seed on first show of the
    // session. The save-on-change path below picks up subsequent drags.
    int savedX = 0, savedY = 0;
    GetMicIconPos(&savedX, &savedY);
    if (savedX <= 0 && savedY <= 0) { savedX = 32; savedY = 32; }
    ImGui::SetNextWindowPos(ImVec2((float)savedX, (float)savedY),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(56, 56), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##l2voice_min", nullptr, flags)) {
        ImGui::End();
        return;
    }
    // Save the icon's position to voice.ini whenever it diverges from
    // what's stored, throttled to once per 250ms.
    {
        static int writtenX = INT_MIN;
        static int writtenY = INT_MIN;
        static int64_t lastWriteMs = 0;
        if (writtenX == INT_MIN) {
            GetMicIconPos(&writtenX, &writtenY);
        }
        ImVec2 cur = ImGui::GetWindowPos();
        int cx = (int)cur.x, cy = (int)cur.y;
        if (cx != writtenX || cy != writtenY) {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - lastWriteMs > 250) {
                SaveMicIconPos(cx, cy);
                writtenX = cx;
                writtenY = cy;
                lastWriteMs = now_ms;
            }
        }
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
        // Texture pre-tinted to white; runtime tint applies gold.
        ImU32 tint = hovered
            ? IM_COL32(0xff, 0xd6, 0x60, 0xff)   // bright gold
            : IM_COL32(0xd4, 0xaf, 0x37, 0xff);  // dim gold
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

// Toast notifications — stack at top-right of the screen below the
// mode banner. Fade-in (200ms), solid display, fade-out (last 500ms
// of TTL). Drawn on the foreground drawlist so they're always on top
// of game + every other overlay element.
void DrawToasts() {
    OverlayToast toasts[8];
    size_t n = GetActiveToasts(toasts, 8);
    if (n == 0) return;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float toastW = 320.0f;
    const float toastH = 48.0f;
    const float spacing = 8.0f;
    // Start below the mode banner zone (banner=28px + 8px margin + 8px gap).
    float rightX = vp->Pos.x + vp->Size.x - toastW - 12.0f;
    float y      = vp->Pos.y + 44.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (size_t i = 0; i < n; ++i) {
        const OverlayToast& t = toasts[i];
        int64_t age   = now_ms - t.created_ms;
        int64_t left  = t.dismiss_ms - now_ms;
        // Alpha envelope: 0->1 over first 200ms, 1->0 over last 500ms.
        float alpha = 1.0f;
        if (age < 200)       alpha = (float)age / 200.0f;
        else if (left < 500) alpha = (float)left / 500.0f;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        ImU32 borderCol;
        switch (t.severity) {
            case 1: borderCol = IM_COL32(240, 180,  40, 255); break; // warn (amber)
            case 2: borderCol = IM_COL32(220,  60,  60, 255); break; // error (red)
            case 3: borderCol = IM_COL32( 80, 200, 100, 255); break; // success (green)
            default: borderCol = IM_COL32( 70, 140, 230, 255); break; // info (blue)
        }
        ImU32 bgCol = IM_COL32(0x14, 0x10, 0x10, 235);

        // Apply alpha envelope.
        auto withAlpha = [](ImU32 c, float a) {
            ImU32 base = c & 0x00FFFFFFu;
            ImU32 alphaByte = (ImU32)(((c >> 24) & 0xFF) * a);
            return base | (alphaByte << 24);
        };

        ImVec2 p0(rightX, y);
        ImVec2 p1(rightX + toastW, y + toastH);
        dl->AddRectFilled(p0, p1, withAlpha(bgCol, alpha), 4.0f);
        // Thick left edge in severity color (vertical bar)
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 4, p1.y),
                          withAlpha(borderCol, alpha), 4.0f);
        dl->AddRect(p0, p1, withAlpha(borderCol, alpha * 0.7f), 4.0f, 0, 1.0f);

        // Text — wrap if longer than the toast width.
        ImVec2 textPos(p0.x + 12, p0.y + 10);
        ImU32 textCol = withAlpha(IM_COL32(0xee, 0xe4, 0xd0, 255), alpha);
        dl->AddText(nullptr, 0.0f, textPos, textCol, t.text, nullptr,
                    toastW - 16.0f);

        y += toastH + spacing;
    }
}

// Top-of-screen banner that surfaces the current clan operational
// mode (PVP / Siege / Boss / Farm). Visible to every clan + ally
// member while a mode is active, because the override semantics of
// Prompt §Regra 6 affect their CLAN/ALLY channel hearing.
//
// Color per Prompt: PVP=red, Siege=purple, Boss=orange, Farm=green.
// PVP and Siege pulse subtly (gentle alpha modulation) so the user
// notices the state change without it becoming visually fatiguing.
void DrawModeBanner() {
    uint8_t mode = GetLocalClanMode();
    if (mode == 0) return;

    const char* modeName;
    ImU32 bg;
    switch (mode) {
        case 1: modeName = "PVP";   bg = IM_COL32(220,  50,  50, 230); break;
        case 2: modeName = "SIEGE"; bg = IM_COL32(160,  80, 200, 230); break;
        case 3: modeName = "BOSS";  bg = IM_COL32(240, 140,  30, 230); break;
        case 4: modeName = "FARM";  bg = IM_COL32( 80, 200, 100, 230); break;
        default: return;
    }

    // Gentle pulse for PVP/Siege so it reads as "alert" without being
    // an actual eye-burner. Other modes stay steady.
    bool pulse = (mode == 1 || mode == 2);
    if (pulse) {
        float t = (float)ImGui::GetTime();
        float a = 0.7f + 0.3f * std::sin(t * 3.5f);
        if (a < 0.4f) a = 0.4f;
        bg = (bg & 0x00FFFFFF) | (ImU32)(a * 230) << 24;
    }

    // Centered at top of main viewport. Banner is input-passthrough
    // so it never grabs clicks meant for the game / other overlays.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bannerW = 300.0f;
    const float bannerH = 28.0f;
    ImVec2 pos(vp->Pos.x + (vp->Size.x - bannerW) * 0.5f,
               vp->Pos.y + 8.0f);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(bannerW, bannerH));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoBackground
                           | ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("##l2voice_mode_banner", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 p0 = ImGui::GetWindowPos();
        ImVec2 p1 = ImVec2(p0.x + bannerW, p0.y + bannerH);
        dl->AddRectFilled(p0, p1, bg, 6.0f);
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 220), 6.0f, 0, 1.5f);

        char text[64];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "CLAN MODE: %s", modeName);
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImVec2 tp(p0.x + (bannerW - ts.x) * 0.5f,
                  p0.y + (bannerH - ts.y) * 0.5f);
        // Soft shadow + bright fg so it pops over any L2 background.
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(tp, IM_COL32(255, 255, 255, 255), text);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// Floating list of nearby/recent speakers shown while the panel is
// minimized — quick-access mute toggle without restoring the full
// panel. Each row: state-colored mic icon + character name. Clicking
// the name toggles local mute (same SetSpeakerMuted as the full panel).
//
// State mapping (per microfone_*.png assets):
//   muted by us            -> microfone_block.png  (red w/ X)
//   currently speaking     -> microfone_falando.png (green solid)
//   silent / not speaking  -> microfone_mudo.png    (green outline)
//
// Layout: small bordered window with no title bar. NoSavedSettings so
// position is per-session; user can drag it anywhere from the icon.
// Anchored once on first show, then ImGui remembers within the session.
void DrawMinimizedSpeakerList() {
    SpeakerInfo speakers[64];
    size_t n = 0;
    GetSpeakerList(speakers, 64, n);
    int ping = GetVoicePingMs();
    // Show the window when there are speakers OR when we have a ping
    // to display — the user wants the ping always visible while the
    // panel is minimized.
    if (n == 0 && ping < 0) return;

    // Restore the last-saved position from voice.ini. If nothing is
    // saved (fresh install), seed at a sensible spot near the icon.
    int savedX = 0, savedY = 0;
    GetMiniListPos(&savedX, &savedY);
    if (savedX <= 0 && savedY <= 0) { savedX = 80; savedY = 80; }
    ImGui::SetNextWindowPos(ImVec2((float)savedX, (float)savedY),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 0), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.04f, 0.04f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    if (!ImGui::Begin("##l2voice_mini_speakers", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }
    // Persist position whenever it diverges from what's currently in
    // voice.ini. Throttled to at most one write per 250ms so a drag
    // doesn't hammer WritePrivateProfileString. Initialized lazily on
    // first frame from the saved value.
    {
        static int      writtenX = INT_MIN;
        static int      writtenY = INT_MIN;
        static int64_t  lastWriteMs = 0;
        if (writtenX == INT_MIN) {
            GetMiniListPos(&writtenX, &writtenY);
        }
        ImVec2 cur = ImGui::GetWindowPos();
        int cx = (int)cur.x, cy = (int)cur.y;
        if (cx != writtenX || cy != writtenY) {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - lastWriteMs > 250) {
                SaveMiniListPos(cx, cy);
                writtenX = cx;
                writtenY = cy;
                lastWriteMs = now_ms;
            }
        }
    }

    const float iconSz = 18.0f;
    for (size_t i = 0; i < n; ++i) {
        const SpeakerInfo& sp = speakers[i];
        bool speaking = sp.ms_since_mix < 200;
        bool muted    = sp.muted;

        ImGui::PushID((int)sp.src_id);

        // Name (left). Clickable -> toggle mute. Resolve the character
        // name via the existing speaker-name cache (sid → name).
        char name[48];
        bool haveName = GetSpeakerName(sp.src_id, name, sizeof(name));

        // Color the name dimmer when muted; bright green when speaking.
        ImVec4 nameCol;
        if (muted)         nameCol = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        else if (speaking) nameCol = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
        else               nameCol = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, nameCol);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
            ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,
            ImVec4(0xd4/255.f, 0xaf/255.f, 0x37/255.f, 0.40f));
        const char* label = (haveName && name[0]) ? name : "speaker";
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
                ImVec2(220.0f - iconSz - 16.0f, 0))) {
            SetSpeakerMuted(sp.src_id, !muted);
            if (muted) SetSpeakerVolume(sp.src_id, 1.0f);
            else       SetSpeakerVolume(sp.src_id, 0.0f);
        }
        ImGui::PopStyleColor(3);

        // Mic icon on the same line, right side.
        ImGui::SameLine();
        IDirect3DTexture9* tex = nullptr;
        if (muted)              tex = g_micBlockedTex;
        else if (speaking)      tex = g_micSpeakingTex;
        else                    tex = g_micMutedTex;
        if (tex) {
            ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                         ImVec2(iconSz, iconSz));
        } else {
            ImGui::TextUnformatted(speaking ? "*" : muted ? "x" : "-");
        }
        ImGui::PopID();
    }

    // Ping line — sit just below the speaker list. Color the value by
    // health: green <100, amber <250, red beyond.
    if (n > 0) ImGui::Separator();
    ImVec4 pingCol;
    if      (ping < 0)   pingCol = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    else if (ping < 100) pingCol = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    else if (ping < 250) pingCol = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
    else                 pingCol = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
    ImGui::TextDisabled("PING:");
    ImGui::SameLine();
    if (ping < 0) ImGui::TextColored(pingCol, "--");
    else          ImGui::TextColored(pingCol, "%d ms", ping);

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void DrawPanel() {
    // The mode banner is independent of panel visibility — it stays
    // pinned at top-of-screen whenever a clan mode is active, even if
    // the user has the main panel hidden or minimized.
    DrawModeBanner();
    // Toasts likewise — always rendered while there are active ones.
    DrawToasts();

    if (g_minimized.load()) {
        DrawMinimized();
        DrawMinimizedSpeakerList();
        return;
    }

    OverlayState st = SnapshotOverlayState();

    // Fixed-size window. Title bar enabled (= draggable, shows
    // connection status), no collapse triangle, no resize grip. The
    // minimize "_" button is rendered manually as an overlay on the
    // title-bar pixels (see below).
    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_Always);
    char titleBuf[64];
    _snprintf_s(titleBuf, sizeof(titleBuf), _TRUNCATE,
        "l2voice  %s###l2voice_window",
        st.ws_connected ? "[connected]" : "[offline]");
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse
                            | ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin(titleBuf, nullptr, wflags)) {
        ImGui::End();
        return;
    }

    // ---- Minimize button on the title bar ----
    // Visuals: ForegroundDrawList (= no per-window clip rect, always
    // on top of everything ImGui has drawn so far).
    // Hit-test: direct io.MousePos + io.MouseClicked (no ImGui item
    // system, so the title-bar drag handler can't "steal" the click).
    //
    // The button rect is computed from the live window pos so it
    // tracks the panel as the user drags it.
    {
        ImGuiStyle& s = ImGui::GetStyle();
        const float titleH = ImGui::GetFontSize() + s.FramePadding.y * 2;
        const float btnSz = titleH;
        ImVec2 winP = ImGui::GetWindowPos();
        float  winW = ImGui::GetWindowWidth();
        ImVec2 btnMin(winP.x + winW - btnSz - 4, winP.y);
        ImVec2 btnMax(btnMin.x + btnSz, btnMin.y + btnSz);

        ImGuiIO& io = ImGui::GetIO();
        bool hovered = (io.MousePos.x >= btnMin.x && io.MousePos.x < btnMax.x
                     && io.MousePos.y >= btnMin.y && io.MousePos.y < btnMax.y);
        if (hovered && io.MouseClicked[0]) {
            OutputDebugStringA("[l2voice] minimize button clicked\n");
            g_minimized.store(true);
        }

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImU32 bg = hovered
            ? IM_COL32(0xd4, 0xaf, 0x37, 0x88)
            : IM_COL32(0x2a, 0x1f, 0x15, 0xee);
        dl->AddRectFilled(btnMin, btnMax, bg, 3.0f);
        ImU32 border = hovered
            ? IM_COL32(0xff, 0xd6, 0x60, 0xff)
            : IM_COL32(0xd4, 0xaf, 0x37, 0xff);
        dl->AddRect(btnMin, btnMax, border, 3.0f, 0, 1.5f);
        float midY = btnMax.y - 5;
        dl->AddLine(ImVec2(btnMin.x + 5, midY),
                    ImVec2(btnMax.x - 5, midY),
                    border, 2.0f);
    }

    // ====== Session + player name ======
    // Both rows right-anchor their chip to the same X so the values
    // line up visually regardless of the chip text width.
    const float chipColumnW = 120.0f;  // reserved width for the chip area
    const float chipX = ImGui::GetWindowWidth() - chipColumnW - 8.0f;

    char sidLabel[24];
    _snprintf_s(sidLabel, sizeof(sidLabel), _TRUNCATE, "sid %u", st.session_id);
    ImGui::TextDisabled("session");
    ImGui::SameLine(chipX);
    Chip(sidLabel);

    ImGui::TextDisabled("player");
    ImGui::SameLine(chipX);
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

    // ====== TX channel indicator ======
    // Always visible near the top so the user knows which channel
    // they're about to broadcast to when they hit PTT.
    {
        static const char* kChanNames[] = {"Proximity", "Party", "Clan", "Ally", "CC"};
        int tx = GetActiveTxChannel();
        if (tx < 0 || tx > 4) tx = 0;
        ImGui::TextDisabled("TX:");
        ImGui::SameLine();
        ImVec4 col = (tx == 0)
            ? ImVec4(180/255.f, 180/255.f, 180/255.f, 1.0f)
            : ImVec4(74/255.f, 222/255.f, 128/255.f, 1.0f);
        ImGui::TextColored(col, "%s", kChanNames[tx]);
        // When the local player is leader/sub-leader AND their clan
        // has an active mode, any TX on Clan/Ally is auto-rewritten
        // by the server to the unified-mode channel with override
        // (Prompt §Regra 6). Show that so the user knows the mode is
        // affecting them.
        uint8_t role = GetLocalRole();
        uint8_t cm   = GetLocalClanMode();
        // Only flag override when there's actually a mode active AND
        // the user is transmitting on a channel that the mode unifies
        // (clan/ally). Otherwise the hint is noise.
        if (role >= 1 && cm != 0 && (tx == 2 || tx == 3)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f),
                "(unified mode)");
        }
    }
    ImGui::Separator();

    // ====== Tabs ======
    if (ImGui::BeginTabBar("##chs")) {
        if (ImGui::BeginTabItem("Proximity")) {
            DrawProximityTab(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Party")) {
            DrawGroupTab(st, 1, "Party");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Clan")) {
            DrawGroupTab(st, 2, "Clan");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Ally")) {
            DrawGroupTab(st, 3, "Ally");
            ImGui::EndTabItem();
        }
        // CC tab only shown when the server says we're in a command
        // channel. Visibility is the spec — "a aba CC só deve aparecer
        // se existir um comando channel."
        if (GetCCID() != 0) {
            if (ImGui::BeginTabItem("CC")) {
                DrawGroupTab(st, 4, "Command Channel");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Speak permission");
                if (!GetCCCanSpeak()) {
                    ImGui::TextColored(
                        ImVec4(0.85f, 0.55f, 0.20f, 1.0f),
                        "  Speak LOCKED — the CC leader hasn't granted you.");
                } else {
                    ImGui::TextColored(
                        ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                        "  Speak unlocked.");
                }
                // CC leader sees a grant/revoke toggle per member.
                uint32_t myPid = st.player_id;
                if (myPid != 0 && myPid == GetCCLeaderID()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Leader controls");
                    OverlayMember roster[64]; size_t n = GetGroupRoster(4, roster, 64);
                    ImGui::BeginChild("##cc_grant", ImVec2(0, 100), true);
                    for (size_t i = 0; i < n; ++i) {
                        if (roster[i].player_id == myPid) continue;
                        ImGui::PushID((int)roster[i].player_id);
                        char nm[48];
                        bool haveN = GetPlayerName(roster[i].player_id, nm, sizeof(nm));
                        bool granted = roster[i].cc_can_speak;
                        if (ImGui::Checkbox("##grant", &granted)) {
                            SendCCGrantSpeak(roster[i].player_id, granted);
                        }
                        ImGui::SameLine();
                        if (haveN && nm[0]) ImGui::Text("%s", nm);
                        else                ImGui::Text("pid=%u", roster[i].player_id);
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    // ---- Leader panel for Clan (inline at bottom of the main panel) ----
    if (GetLocalRole() >= 1 /*sub-leader or leader*/) {
        ImGui::Separator();
        ImGui::TextDisabled("Clan leader controls");
        static const char* modeNames[] = {
            "None", "PVP", "Siege", "Boss", "Farm",
        };
        int currentMode = SnapshotOverlayState().ws_connected
            ? (int)(uint8_t)0 // mode comes from VoiceNetwork::LocalClanMode below
            : 0;
        // Actually read live mode (LocalClanMode is on VoiceNetwork, exposed
        // through GetLocalRole? add a getter via GetLocalClanMode).
        currentMode = (int)0;
        // We don't yet expose local clan mode through a getter; show
        // mode buttons that send-only. (Server broadcasts back the new
        // mode via client_state -> mode banner will reflect it.)
        for (int i = 0; i < 5; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(modeNames[i])) {
                SendClanSetMode((uint8_t)i);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Insert hides");
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
            int slot = g_captureNextSlot.load();
            g_captureNextKey.store(false);
            switch (slot) {
                case 0: SetPttProximityVk(capturedVk); break;
                case 1: SetPttPartyVk(capturedVk);     break;
                case 2: SetPttClanVk(capturedVk);      break;
                case 3: SetPttAllyVk(capturedVk);      break;
                default: SetPttProximityVk(capturedVk); break;
            }
            Logf("[l2voice] PTT slot=%d rebound to vk=%d\n", slot, capturedVk);
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

        // Mic icon — embedded as RCDATA in the DLL (see
        // voice/resources.rc.in). Pre-tinted to WHITE so the runtime
        // tint controls the displayed color (gold base, brighter on
        // hover). No file dependency at runtime.
        g_micTexture = LoadEmbeddedMicTexture(dev, g_micW, g_micH,
            /*tintRgb*/ 0xFFFFFF);
        if (g_micTexture) {
            Logf("[l2voice] icon loaded from resource: %dx%d\n",
                g_micW, g_micH);
        } else {
            Logf("[l2voice] embedded icon resource not found — text fallback\n");
        }
        // State-colored mic icons for the minimized speaker overlay.
        // Preserve original PNG colors (tint=0).
        int wTmp, hTmp;
        g_micSpeakingTex = LoadEmbeddedPng(dev, IDR_MIC_SPEAKING_PNG, wTmp, hTmp, 0);
        g_micMutedTex    = LoadEmbeddedPng(dev, IDR_MIC_MUTED_PNG,    wTmp, hTmp, 0);
        g_micBlockedTex  = LoadEmbeddedPng(dev, IDR_MIC_BLOCKED_PNG,  wTmp, hTmp, 0);
        Logf("[l2voice] state mics loaded: speak=%p muted=%p blocked=%p\n",
             (void*)g_micSpeakingTex, (void*)g_micMutedTex, (void*)g_micBlockedTex);
        // L2UI_CH3 buttons (native L2 textures — original PNG colors).
        g_l2BtnBig         = LoadEmbeddedPng(dev, IDR_L2UI_BTN_BIG,           wTmp, hTmp, 0);
        g_l2BtnBigOver     = LoadEmbeddedPng(dev, IDR_L2UI_BTN_BIG_OVER,      wTmp, hTmp, 0);
        g_l2BtnBigDown     = LoadEmbeddedPng(dev, IDR_L2UI_BTN_BIG_DOWN,      wTmp, hTmp, 0);
        g_l2BtnSmall       = LoadEmbeddedPng(dev, IDR_L2UI_BTN_SMALL,         wTmp, hTmp, 0);
        g_l2BtnSmallOver   = LoadEmbeddedPng(dev, IDR_L2UI_BTN_SMALL_OVER,    wTmp, hTmp, 0);
        g_l2BtnSmallDown   = LoadEmbeddedPng(dev, IDR_L2UI_BTN_SMALL_DOWN,    wTmp, hTmp, 0);
        g_l2FrameMini      = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_MINI,        wTmp, hTmp, 0);
        g_l2FrameMiniOver  = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_MINI_OVER,   wTmp, hTmp, 0);
        g_l2FrameMiniDown  = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_MINI_DOWN,   wTmp, hTmp, 0);
        g_l2FrameClose     = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_CLOSE,       wTmp, hTmp, 0);
        g_l2FrameCloseOver = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_CLOSE_OVER,  wTmp, hTmp, 0);
        g_l2FrameCloseDown = LoadEmbeddedPng(dev, IDR_L2UI_FRAME_CLOSE_DOWN,  wTmp, hTmp, 0);
        g_l2TabSelected         = LoadEmbeddedPng(dev, IDR_L2UI_TAB_SELECTED,         wTmp, hTmp, 0);
        g_l2TabUnselected       = LoadEmbeddedPng(dev, IDR_L2UI_TAB_UNSELECTED,       wTmp, hTmp, 0);
        g_l2TabUnselectedOver   = LoadEmbeddedPng(dev, IDR_L2UI_TAB_UNSELECTED_OVER,  wTmp, hTmp, 0);
        g_l2WndBg               = LoadEmbeddedPng(dev, IDR_L2UI_WND_BG,               wTmp, hTmp, 0);
        Logf("[l2voice] L2UI_CH3 textures loaded: big=%p small=%p mini=%p close=%p tabs=%p/%p/%p\n",
             (void*)g_l2BtnBig, (void*)g_l2BtnSmall, (void*)g_l2FrameMini,
             (void*)g_l2FrameClose,
             (void*)g_l2TabSelected, (void*)g_l2TabUnselected, (void*)g_l2TabUnselectedOver);

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
