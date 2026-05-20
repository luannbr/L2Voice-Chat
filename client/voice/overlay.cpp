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
HHOOK           g_mouseHook    = nullptr;
ImGuiContext*   g_imguiCtx     = nullptr;
std::atomic<bool> g_imguiBackendInit{false};
std::atomic<bool> g_visible{true};
std::atomic<bool> g_minimized{false};
std::atomic<bool> g_imguiWantsMouse{false};   // sampled each frame; read by low-level hook
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

void ApplyDarkGlassStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 8.0f;
    s.FrameRounding    = 6.0f;
    s.GrabRounding     = 6.0f;
    s.WindowPadding    = ImVec2(12, 10);
    s.ItemSpacing      = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabRounding      = 6.0f;

    ImVec4 bg     = ImVec4(20/255.f, 22/255.f, 30/255.f, 0.92f);
    ImVec4 border = ImVec4(1,1,1, 0.08f);
    ImVec4 text   = ImVec4(232/255.f, 234/255.f, 240/255.f, 1.0f);
    ImVec4 textD  = ImVec4(139/255.f, 146/255.f, 163/255.f, 1.0f);
    ImVec4 accent = ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f);
    ImVec4 accentBg     = ImVec4(129/255.f, 140/255.f, 248/255.f, 0.15f);
    ImVec4 accentHover  = ImVec4(129/255.f, 140/255.f, 248/255.f, 0.25f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]              = bg;
    c[ImGuiCol_ChildBg]               = ImVec4(0,0,0,0);
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
    c[ImGuiCol_Tab]                   = ImVec4(1,1,1, 0.04f);
    c[ImGuiCol_TabHovered]            = accentHover;
    c[ImGuiCol_TabActive]             = accentBg;
    c[ImGuiCol_TabUnfocused]          = ImVec4(1,1,1, 0.02f);
    c[ImGuiCol_TabUnfocusedActive]    = accentBg;
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
// Minimized state
// =============================================================

void DrawMinimized() {
    ImGui::SetNextWindowSize(ImVec2(48, 48), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("##l2voice_min", nullptr, flags)) {
        ImGui::End();
        return;
    }
    ImGui::SetCursorPos(ImVec2(10, 10));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(129/255.f, 140/255.f, 248/255.f, 1.0f));
    ImGui::TextUnformatted("VOX");
    ImGui::PopStyleColor();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_minimized.store(false);
    }
    ImGui::End();
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

void DrawPanel() {
    if (!g_visible.load(std::memory_order_relaxed)) return;

    OverlayState st = SnapshotOverlayState();
    if (st.session_id == 0) {
        g_imguiWantsMouse.store(false);
        return;
    }

    if (g_minimized.load()) {
        DrawMinimized();
        g_imguiWantsMouse.store(ImGui::GetIO().WantCaptureMouse);
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 430), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("##l2voice", nullptr, flags)) {
        ImGui::End();
        g_imguiWantsMouse.store(false);
        return;
    }

    // ====== Header (3-column layout: title | spacer | controls) ======
    // Render controls right-to-left from the window's right edge so
    // the minimize button is always visible regardless of text width.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1, 1.0f));
    ImGui::TextUnformatted("l2voice");
    ImGui::PopStyleColor();

    // Right-anchor block: [ minimize ] [ status ] [ dot ]
    const char* statusText = st.ws_connected ? "connected" : "offline";
    float minBtnW    = ImGui::CalcTextSize("_").x  + 18.0f;
    float statusW    = ImGui::CalcTextSize(statusText).x + 6.0f;
    float dotW       = 14.0f;
    float headerW    = minBtnW + statusW + dotW + 12.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - headerW - 8.0f);
    DrawConnectionDot(st.ws_connected);
    ImGui::TextDisabled("%s", statusText);
    ImGui::SameLine();
    if (ImGui::SmallButton("_##min")) g_minimized.store(true);
    ImGui::Separator();

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
    ImGui::TextDisabled("Insert to hide  ·  _ to minimize");

    // Snapshot WantCaptureMouse for the low-level hook (read on
    // another thread without grabbing the ImGui context).
    g_imguiWantsMouse.store(ImGui::GetIO().WantCaptureMouse);

    ImGui::End();
}

// =============================================================
// Low-level mouse hook: blocks mouse input from reaching L2 (via
// DirectInput etc.) when ImGui is the intended consumer.
// =============================================================

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode != HC_ACTION) return CallNextHookEx(nullptr, nCode, wp, lp);
    if (!g_imguiBackendInit.load() || !g_imguiWantsMouse.load()) {
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }
    // Only block when the cursor is over the L2 window and the panel
    // is the intended target. We let mouse movement through so the
    // OS cursor still follows, but suppress button presses.
    if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP ||
        wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP ||
        wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP ||
        wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP ||
        wp == WM_MOUSEWHEEL  || wp == WM_MOUSEHWHEEL) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            HWND under = WindowFromPoint(pt);
            DWORD ownerPid = 0;
            if (under) GetWindowThreadProcessId(under, &ownerPid);
            if (ownerPid == GetCurrentProcessId()) {
                return 1;   // swallow — game (DirectInput too) won't see it
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
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

        if (msg == WM_SETCURSOR && io.WantCaptureMouse) {
            ::SetCursor(::LoadCursorA(nullptr, IDC_ARROW));
            return TRUE;
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
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;

        ApplyDarkGlassStyle();

        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(&HookedWndProc)));

        // Install the low-level mouse hook now that we know we're
        // in-process and have a context. WH_MOUSE_LL is per-thread when
        // hMod is NULL and ThreadId is 0; but it's global with a
        // non-null hMod. We use ourselves so the callback lives in our
        // module's address space — required for LL hooks.
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&HookedWndProc), &self);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &LowLevelMouseProc,
            self, 0);
        if (!g_mouseHook) {
            Logf("[l2voice] overlay: WH_MOUSE_LL install failed (err=%lu)\n",
                GetLastError());
        }

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
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
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
