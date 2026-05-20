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
// game.

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

// Forward-declared by imgui_impl_win32.h via extern "C" macro.
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
std::atomic<bool> g_visible{true};       // toggle with Insert
int             g_toggleVk      = VK_INSERT;
std::atomic<bool> g_captureNextKey{false};  // PTT rebind: capture next WM_KEYDOWN

// Renders a VK code as a readable label (e.g., "H", "Mouse 4",
// "F1"). Falls back to "vk=N" for codes we don't have a mnemonic
// for. Mostly used by the overlay's PTT display + rebind UI.
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

void Logf(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

void DrawPanel() {
    if (!g_visible.load(std::memory_order_relaxed)) return;

    OverlayState st = SnapshotOverlayState();

    // Stay hidden until the player has actually entered the world.
    // session_id is set in the auth_ok handler, which only fires after
    // the bridge resolves our player_id from the GS TCP table → that
    // happens once the L2 client is past character-select and in-game.
    if (st.session_id == 0) return;

    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(340, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("l2voice", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // ---- Status ----
    ImGui::Text("ws: %s   sid=%u   player=%u",
        st.ws_connected ? "connected" : "DISCONNECTED",
        st.session_id, st.player_id);
    ImGui::Separator();

    // ---- Master volume ----
    float vol = st.master_volume;
    if (ImGui::SliderFloat("master volume", &vol, 0.0f, 2.0f, "%.2f")) {
        SetMasterVolume(vol);
    }

    // ---- Capture toggles ----
    bool focus = st.require_focus;
    if (ImGui::Checkbox("require window focus", &focus)) {
        SetRequireFocus(focus);
    }
    bool on = st.always_on;
    if (ImGui::Checkbox("always on (no PTT)", &on)) {
        SetAlwaysOn(on);
    }

    // ---- PTT rebind ----
    bool capturing = g_captureNextKey.load();
    if (capturing) {
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
            "PRESS ANY KEY OR MOUSE BUTTON (Esc to cancel)");
    } else {
        char vkLabel[32];
        VkToString(st.ptt_proximity_vk, vkLabel, sizeof(vkLabel));
        ImGui::Text("PTT key: %s", vkLabel);
        ImGui::SameLine();
        if (ImGui::SmallButton("rebind")) {
            g_captureNextKey.store(true);
        }
    }
    ImGui::Separator();

    // ---- Speaker list with mute checkboxes ----
    ImGui::Text("speakers (%d active):", st.active_speakers);
    SpeakerInfo infos[16];
    size_t n = 0;
    GetSpeakerList(infos, 16, n);
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
        ImVec4 col = speaking ? ImVec4(0.4f, 1.f, 0.4f, 1.f)
                              : ImVec4(0.6f, 0.6f, 0.6f, 1.f);
        char name[48];
        bool haveName = GetSpeakerName(infos[i].src_id, name, sizeof(name));
        if (haveName) {
            ImGui::TextColored(col, "%s  gain=%.2f%s",
                name, infos[i].gain, speaking ? "  <speaking>" : "");
        } else {
            ImGui::TextColored(col, "sid=%u  gain=%.2f%s",
                infos[i].src_id, infos[i].gain,
                speaking ? "  <speaking>" : "");
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Insert to hide");
    ImGui::End();
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Show/hide hotkey, before ImGui sees it.
    if (msg == WM_KEYDOWN && (int)wp == g_toggleVk) {
        bool prev = g_visible.exchange(!g_visible.load());
        Logf("[l2voice] overlay toggled %s\n", prev ? "OFF" : "ON");
        return 0;
    }
    // PTT rebind capture. Accepts:
    //   - WM_KEYDOWN (any non-modifier key, Esc cancels)
    //   - WM_RBUTTONDOWN / WM_MBUTTONDOWN / WM_XBUTTONDOWN (mouse buttons
    //     other than LMB — LMB is excluded because it's also how the
    //     user clicked the "rebind" button)
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
        if (cancel) {
            g_captureNextKey.store(false);
            return 0;
        }
        if (capturedVk != 0) {
            g_captureNextKey.store(false);
            SetPttProximityVk(capturedVk);
            Logf("[l2voice] PTT rebound to vk=%d\n", capturedVk);
            return 0;
        }
    }
    if (g_imguiBackendInit.load() &&
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return true;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

HRESULT WINAPI HookEndScene(IDirect3DDevice9* dev) {
    if (!g_imguiBackendInit.load()) {
        // First call — set up backends using the real device.
        ImGui::SetCurrentContext(g_imguiCtx);

        // Find the L2 window so the input hook is on the right HWND.
        // Trust the device's creation params first; fall back to the
        // current foreground window of this process.
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
        io.IniFilename = nullptr;   // no imgui.ini sidecar
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

// Creates a throwaway IDirect3DDevice9 on the desktop so we can read
// the vtable. Releases on return.
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
    resetOut    = vt[16];   // IDirect3DDevice9::Reset
    endSceneOut = vt[42];   // IDirect3DDevice9::EndScene
    dev->Release();
    d3d->Release();
    return true;
}

}  // namespace

bool InstallOverlay() {
    if (g_imguiCtx) return true;  // already installed
    void* endSceneAddr = nullptr;
    void* resetAddr    = nullptr;
    if (!GetDeviceVTableEntries(endSceneAddr, resetAddr)) {
        Logf("[l2voice] overlay: GetDeviceVTableEntries failed\n");
        return false;
    }
    Logf("[l2voice] overlay: EndScene=%p Reset=%p\n", endSceneAddr, resetAddr);

    // MinHook may already be initialized elsewhere — treat already-init
    // as a success.
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("[l2voice] overlay: MH_Initialize failed: %d\n", s);
        return false;
    }
    if (MH_CreateHook(endSceneAddr,
            reinterpret_cast<void*>(&HookEndScene),
            reinterpret_cast<void**>(&g_origEndScene)) != MH_OK) {
        Logf("[l2voice] overlay: MH_CreateHook(EndScene) failed\n");
        return false;
    }
    if (MH_CreateHook(resetAddr,
            reinterpret_cast<void*>(&HookReset),
            reinterpret_cast<void**>(&g_origReset)) != MH_OK) {
        Logf("[l2voice] overlay: MH_CreateHook(Reset) failed\n");
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("[l2voice] overlay: MH_EnableHook failed\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    g_imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imguiCtx);
    ImGui::StyleColorsDark();

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
