// dllmain.cpp — entry point for l2voice.dll.
//
// Separate from l2ui/AutoLogin. Loaded into the L2 client process via
// Engine.dll IAT injection (Themida is on L2.exe and stays untouched
// — see project-l2-interlude-devicelost-bug memory note).
//
// On attach: spawn an init thread (DllMain can't do non-trivial work
// while the loader lock is held). That thread:
//   1. resolves voice.ini path next to this DLL
//   2. loads config (or DefaultConfig if absent)
//   3. calls voice::Init(cfg)
//   4. spawns a 50 Hz polling thread that calls
//      voice::OnRenderFrame() — refreshes local-player state cache.
//      This avoids needing our own D3D9 hook for the MVP.
//
// On detach: signal stop, join the poll thread, call voice::Shutdown.

#include "voice/voice.h"

#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <atomic>
#include <thread>

namespace {

HMODULE         g_module = nullptr;
std::atomic<bool> g_running{false};
std::thread     g_poll_thread;
std::thread     g_init_thread;

void PollLoop() {
    // 50 Hz cadence: matches the 20ms outgoing audio frame interval,
    // so each capture frame stamps a coordinate that's at most ~20ms
    // stale. Cheap — RefreshLocalPlayerState is a few __try reads.
    const auto period = std::chrono::milliseconds(20);
    auto next = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)) {
        voice::OnRenderFrame();
        next += period;
        std::this_thread::sleep_until(next);
    }
}

void ResolveIniPath(wchar_t* out, size_t cap) {
    GetModuleFileNameW(g_module, out, (DWORD)cap);
    PathRemoveFileSpecW(out);
    wcscat_s(out, cap, L"\\voice.ini");
}

void InitWorker() {
    wchar_t ini_path[MAX_PATH];
    ResolveIniPath(ini_path, MAX_PATH);

    voice::Config cfg;
    if (!voice::LoadConfigFromIni(ini_path, &cfg)) {
        cfg = voice::DefaultConfig();
    }
    if (!cfg.enabled) return;          // honor the flag; DLL stays inert

    if (!voice::Init(cfg)) {
        OutputDebugStringA("[l2voice] voice::Init failed\n");
        return;
    }

    g_running.store(true, std::memory_order_release);
    g_poll_thread = std::thread(PollLoop);
}

void Shutdown() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) {
        // Init never completed; nothing to tear down.
        if (g_init_thread.joinable()) g_init_thread.detach();
        return;
    }
    if (g_poll_thread.joinable()) g_poll_thread.join();
    voice::Shutdown();
    if (g_init_thread.joinable()) g_init_thread.detach();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_module = hModule;
            DisableThreadLibraryCalls(hModule);
            // Do all real work off the loader lock.
            g_init_thread = std::thread(InitWorker);
            break;
        case DLL_PROCESS_DETACH:
            Shutdown();
            break;
    }
    return TRUE;
}
