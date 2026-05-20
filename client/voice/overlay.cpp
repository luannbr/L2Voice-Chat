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
std::atomic<bool> g_visible{true};   // toggle with Insert
int             g_toggleVk     = VK_INSERT;

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

    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("l2voice", nullptr,
            ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // ---- Connection status ----
    ImGui::Text("ws: %s   sid=%u   player=%u",
        st.ws_connected ? "connected" : "DISCONNECTED",
        st.session_id, st.player_id);
    ImGui::Text("active speakers: %d", st.active_speakers);
    ImGui::Separator();

    // ---- Capture mode ----
    bool focus = st.require_focus;
    if (ImGui::Checkbox("require window focus", &focus)) {
        SetRequireFocus(focus);
    }
    bool on = st.always_on;
    if (ImGui::Checkbox("always on (no PTT)", &on)) {
        SetAlwaysOn(on);
    }
    ImGui::Text("PTT key vk=%d", st.ptt_proximity_vk);

    // ---- TODO panels in next iteration: ----
    // - Master volume slider (needs AudioPlayback API)
    // - Per-speaker mute list (needs sid + name resolution)
    // - PTT rebind "press a key" UI

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
