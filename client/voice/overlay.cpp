// overlay.cpp — D3D9 EndScene hook + Dear ImGui in-game panel.
//
// Hooking approach (well-known "dummy device" trick):
//   1. Create a throwaway IDirect3DDevice9 against the desktop HWND.
//   2. Read the vtable entry at index 42 (EndScene) and 16 (Reset).
//   3. Both are SHARED across all devices created by the same D3D9
//      DLL, so hooking those vtable slots intercepts EndScene on
//      L2's real device once it comes up.
//   4. Release the dummy device — only the function addresses are
//      needed.
//
// MinHook detours those addresses to our handlers. On first call we
// initialize ImGui's DX9 + Win32 backends using the real device's
// presentation parameters.
//
// Input: SetWindowLongPtrW(GWLP_WNDPROC) swap to capture mouse/kb,
// chaining to ImGui_ImplWin32_WndProcHandler before passing to the
// game. Mouse/keyboard events ImGui wants are CONSUMED (don't leak
// to the game). WM_SETCURSOR is suppressed while ImGui has the
// mouse so the game's custom cursor doesn't fight the panel's.

#include "overlay.h"
#include "audio_io.h"     // SpeakerInfo
#include "voice.h"

#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

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
ImGuiContext*   g_imguiCtx     = nullptr;
std::atomic<bool> g_imguiBackendInit{false};
std::atomic<bool> g_visible{true};       // Insert toggles
std::atomic<bool> g_minimized{false};    // collapsed to a small icon
int             g_toggleVk      = VK_INSERT;
std::atomic<bool> g_captureNextKey{false};

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

// "Dark glassmorphism" style per the gallery design 01.
// Colors picked off the HTML mock:
//   bg          rgba(20,22,30,0.92)
//   border      rgba(255,255,255,0.08)
//   text        #e8eaf0
//   muted text  #8b92a3
//   accent      #818cf8  (indigo)
//   accent-bg   #818cf8 @ 15% alpha — used for chips
//   dot ok      #4ade80
void ApplyDarkGlassStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 8.0f;
    s.FrameRounding    = 6.0f;
    s.GrabRounding     = 6.0f;
    s.WindowPadding    = ImVec2(12, 10);
    s.ItemSpacing      = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;

    ImVec4 bg     = ImVec4(20/255.f, 22/255.f, 30/255.f, 0.92f);
    ImVec4 border = ImVec4(1,1,1, 0.08f);
    ImVec4 text   = ImVec4(232/255.f, 234/255.f, 240/255.f, 1.0f);
    ImVec4 textD  = ImVec4(139/255.f, 146/255.f, 163/255.f, 1.0f);
    ImVec4 accent = ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f);
    ImVec4 accentBg     = ImVec4(129/255.f, 140/255.f, 248/255.f, 0.15f);
    ImVec4 accentHover  = ImVec4(129/255.f, 140/255.f, 248/255.f, 0.25f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]              = bg;
    c[ImGuiCol_Border]                = border;
    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textD;
    c[ImGuiCol_FrameBg]               = ImVec4(1,1,1, 0.06f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(1,1,1, 0.08f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(1,1,1, 0.10f);
    c[ImGuiCol_TitleBg]               = bg;
    c[ImGuiCol_TitleBgActive]         = bg;
    c[ImGuiCol_TitleBgCollapsed]      = bg;
    c[ImGuiCol_Button]                = accentBg;
    c[ImGuiCol_ButtonHovered]         = accentHover;
    c[ImGuiCol_ButtonActive]          = accent;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accent;
    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_Separator]             = border;
    c[ImGuiCol_SeparatorHovered]      = accentHover;
    c[ImGuiCol_SeparatorActive]       = accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0,0,0,0);
    c[ImGuiCol_ResizeGripHovered]     = accentHover;
    c[ImGuiCol_ResizeGripActive]      = accent;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(1,1,1,0.1f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(1,1,1,0.15f);
}

// Tiny chip/badge — accent-colored rounded rectangle with text.
void Chip(const char* text, ImVec4 color = ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f)) {
    ImVec4 bg = color; bg.w = 0.15f;
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bg);
    ImGui::PushStyleColor(ImGuiCol_Text,          color);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
    ImGui::SmallButton(text);   // SmallButton has no callback path; just visual
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawConnectionDot(bool ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += 6;  // align with text baseline
    ImU32 col = ok ? IM_COL32(74, 222, 128, 255) : IM_COL32(160, 80, 80, 255);
    dl->AddCircleFilled(ImVec2(p.x + 4, p.y), 4.0f, col);
    if (ok) {
        // soft glow
        dl->AddCircleFilled(ImVec2(p.x + 4, p.y), 7.0f,
            IM_COL32(74, 222, 128, 50));
    }
    ImGui::Dummy(ImVec2(12, 0));
    ImGui::SameLine();
}

// =============================================================
// Minimized state — small floating icon
// =============================================================

void DrawMinimized() {
    ImGui::SetNextWindowSize(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("##l2voice_min", nullptr, flags)) {
        ImGui::End();
        return;
    }
    // Centered headphone glyph. Unicode would be nicer but default
    // font doesn't ship emoji — use a stylized "(•)" or letter.
    ImGui::SetCursorPos(ImVec2(8, 5));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f));
    ImGui::Text("VOX");
    ImGui::PopStyleColor();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_minimized.store(false);
    }
    ImGui::End();
}

// =============================================================
// Main panel (Design 01 — Dark Glassmorphism)
// =============================================================

void DrawPanel() {
    if (!g_visible.load(std::memory_order_relaxed)) return;

    OverlayState st = SnapshotOverlayState();
    // Hide entirely until past EnterWorld (session is allocated).
    if (st.session_id == 0) return;

    if (g_minimized.load()) { DrawMinimized(); return; }

    ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("##l2voice", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // ----- Header: title / connection dot / minimize -----
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1, 1.0f));
    ImGui::TextUnformatted("l2voice");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    // right-align connection chip + minimize button
    float rightX = ImGui::GetWindowWidth() - 70;
    ImGui::SameLine(rightX);
    DrawConnectionDot(st.ws_connected);
    ImGui::TextDisabled("%s", st.ws_connected ? "connected" : "offline");
    ImGui::SameLine();
    if (ImGui::SmallButton("_##min")) {
        g_minimized.store(true);
    }
    ImGui::Separator();

    // ----- Session chip row -----
    ImGui::TextDisabled("session");
    ImGui::SameLine(rightX);
    char sidLabel[24];
    _snprintf_s(sidLabel, sizeof(sidLabel), _TRUNCATE, "sid %u", st.session_id);
    Chip(sidLabel);
    if (st.player_id != 0) {
        ImGui::TextDisabled("player");
        ImGui::SameLine(rightX);
        char pidLabel[24];
        _snprintf_s(pidLabel, sizeof(pidLabel), _TRUNCATE, "%u", st.player_id);
        Chip(pidLabel);
    }

    // ----- Master volume -----
    ImGui::Spacing();
    ImGui::TextDisabled("master volume");
    ImGui::SameLine(rightX);
    ImGui::Text("%d%%", (int)(st.master_volume * 100));
    float vol = st.master_volume;
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 2.0f, "", ImGuiSliderFlags_NoInput)) {
        SetMasterVolume(vol);
    }
    ImGui::PopItemWidth();

    // ----- Toggles -----
    ImGui::Spacing();
    bool focus = st.require_focus;
    if (ImGui::Checkbox("require window focus", &focus)) {
        SetRequireFocus(focus);
    }
    bool on = st.always_on;
    if (ImGui::Checkbox("always on (no PTT)", &on)) {
        SetAlwaysOn(on);
    }

    // ----- PTT -----
    ImGui::Spacing();
    ImGui::TextDisabled("push-to-talk");
    ImGui::SameLine(rightX - 40);
    bool capturing = g_captureNextKey.load();
    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(255/255.f, 178/255.f, 51/255.f, 1.0f));
        ImGui::TextUnformatted("press a key");
        ImGui::PopStyleColor();
    } else {
        char vkLabel[32];
        VkToString(st.ptt_proximity_vk, vkLabel, sizeof(vkLabel));
        Chip(vkLabel);
        ImGui::SameLine();
        if (ImGui::SmallButton("rebind")) {
            g_captureNextKey.store(true);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();

    // ----- Speakers -----
    ImGui::TextDisabled("speakers");
    ImGui::SameLine(rightX);
    ImGui::TextDisabled("%d active", st.active_speakers);

    SpeakerInfo infos[16];
    size_t n = 0;
    GetSpeakerList(infos, 16, n);
    if (n == 0) {
        ImGui::Spacing();
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
                              : ImVec4(139/255.f, 146/255.f, 163/255.f, 1.0f);
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

    ImGui::Separator();
    ImGui::TextDisabled("Insert to hide  ·  click _ to minimize");
    ImGui::End();
}

// =============================================================
// WndProc — input routing
// =============================================================

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 1) Show/hide hotkey, before ImGui sees it.
    if (msg == WM_KEYDOWN && (int)wp == g_toggleVk) {
        bool prev = g_visible.exchange(!g_visible.load());
        Logf("[l2voice] overlay toggled %s\n", prev ? "OFF" : "ON");
        return 0;
    }

    // 2) PTT rebind capture (keyboard + mouse buttons except LMB).
    if (g_captureNextKey.load()) {
        int capturedVk = 0;
        bool cancel = false;
        if (msg == WM_KEYDOWN) {
            int vk = (int)wp;
            if (vk == VK_ESCAPE) { cancel = true; }
            else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                     vk != VK_LSHIFT && vk != VK_RSHIFT &&
                     vk != VK_LCONTROL && vk != VK_RCONTROL &&
                     vk != VK_LMENU && vk != VK_RMENU) {
                capturedVk = vk;
            }
        } else if (msg == WM_RBUTTONDOWN) capturedVk = VK_RBUTTON;
        else if (msg == WM_MBUTTONDOWN)  capturedVk = VK_MBUTTON;
        else if (msg == WM_XBUTTONDOWN) {
            capturedVk = (HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        }
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

        // 3) Suppress the game's WM_SETCURSOR while ImGui has the mouse
        //    — otherwise the game's custom cursor fights the panel's
        //    and the cursor flickers.
        if (msg == WM_SETCURSOR && io.WantCaptureMouse) {
            ::SetCursor(::LoadCursorA(nullptr, IDC_ARROW));
            return TRUE;
        }

        // 4) Consume mouse / keyboard messages when ImGui wants them,
        //    so clicks on the panel don't pass through to the game.
        bool isMouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                       msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
        bool isKey   = (msg == WM_KEYDOWN || msg == WM_KEYUP ||
                        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                        msg == WM_CHAR);
        if (isMouse && io.WantCaptureMouse)       return 0;
        if (isKey   && io.WantCaptureKeyboard)    return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// =============================================================
// D3D9 hook
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
        // Tell ImGui to NOT manage the OS cursor — the game owns it.
        // WM_SETCURSOR suppression in WndProc handles UI areas.
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;

        ApplyDarkGlassStyle();

        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(&HookedWndProc)));
        g_imguiBackendInit.store(true);
    }

    ImGui::SetCurrentContext(g_imguiCtx);
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawPanel();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

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
    if (FAILED(hr) || !dev) {
        d3d->Release();
        return false;
    }
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
    if (MH_CreateHook(endSceneAddr,
            reinterpret_cast<void*>(&HookEndScene),
            reinterpret_cast<void**>(&g_origEndScene)) != MH_OK ||
        MH_CreateHook(resetAddr,
            reinterpret_cast<void*>(&HookReset),
            reinterpret_cast<void**>(&g_origReset)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("[l2voice] overlay: hook install failed\n");
        return false;
    }

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
