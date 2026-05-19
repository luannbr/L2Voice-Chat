// dllmain.cpp — entry point for l2voice.dll.
//
// Separate DLL from l2ui/AutoLogin. Loaded into the L2 client process
// via Engine.dll IAT injection (Themida is on L2.exe and stays
// untouched — see project-l2-interlude-devicelost-bug memory note).
//
// On attach: spawn an init thread (DllMain can't do non-trivial work
// while the loader lock is held). That thread:
//   1. resolves voice.ini path next to this DLL
//   2. loads config (or DefaultConfig if absent)
//   3. calls voice::Init(cfg)
//
// Position no longer comes from client memory (protocol rev 2 — L2J
// pushes positions to the voice-service over Redis), so the DLL has
// no per-frame work and does not need its own D3D9 hook.

#include "voice/voice.h"

#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <thread>

namespace {

HMODULE     g_module = nullptr;
std::thread g_init_thread;

void ResolveIniPath(wchar_t* out, size_t cap) {
    GetModuleFileNameW(g_module, out, (DWORD)cap);
    PathRemoveFileSpecW(out);
    wcscat_s(out, cap, L"\\voice.ini");
}

void InitWorker() {
    OutputDebugStringA("[l2voice] InitWorker started\n");
    wchar_t ini_path[MAX_PATH];
    ResolveIniPath(ini_path, MAX_PATH);

    voice::Config cfg;
    bool fromIni = voice::LoadConfigFromIni(ini_path, &cfg);
    if (!fromIni) {
        cfg = voice::DefaultConfig();
    }
    // Env-var override for player_id, scoped to the launching process.
    // Useful for testing two clients out of the same install directory:
    //     set L2VOICE_PLAYER_ID=268499104
    //     L2.exe
    // Whatever is set in voice.ini is used otherwise.
    char envBuf[64];
    size_t envLen = 0;
    if (getenv_s(&envLen, envBuf, sizeof(envBuf), "L2VOICE_PLAYER_ID") == 0
            && envLen > 1) {
        uint32_t envPid = (uint32_t)strtoul(envBuf, nullptr, 10);
        if (envPid != 0) {
            cfg.player_id = envPid;
            char eb[96];
            _snprintf_s(eb, sizeof(eb), _TRUNCATE,
                "[l2voice] L2VOICE_PLAYER_ID env override: %u\n", envPid);
            OutputDebugStringA(eb);
        }
    }
    char dbg[512];
    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
        "[l2voice] config %s enabled=%d auto_connect=%d ws_url=%s ptt=%d player_id=%u\n",
        fromIni ? "from voice.ini" : "default",
        cfg.enabled, cfg.auto_connect, cfg.ws_url, cfg.ptt_proximity, cfg.player_id);
    OutputDebugStringA(dbg);

    if (!cfg.enabled) {
        OutputDebugStringA("[l2voice] disabled by config; bridge inert\n");
        return;
    }

    if (!voice::Init(cfg)) {
        OutputDebugStringA("[l2voice] voice::Init FAILED\n");
        return;
    }
    OutputDebugStringA("[l2voice] voice::Init OK (audio devices opened, ws connecting)\n");
}

void Shutdown() {
    voice::Shutdown();
    if (g_init_thread.joinable()) g_init_thread.detach();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_module = hModule;
            DisableThreadLibraryCalls(hModule);
            OutputDebugStringA("[l2voice] DLL_PROCESS_ATTACH\n");
            g_init_thread = std::thread(InitWorker);
            break;
        case DLL_PROCESS_DETACH:
            OutputDebugStringA("[l2voice] DLL_PROCESS_DETACH\n");
            Shutdown();
            break;
    }
    return TRUE;
}

// ---- public exports -------------------------------------------------
//
// IAT-injection tools need at least one named export so they can write
// an IMPORT_DESCRIPTOR pointing at l2voice.dll!<name>. The body can be
// empty — voice::Init() already runs from DllMain on DLL_PROCESS_ATTACH.
//
// L2Voice_Init   : matches the l2ui.dll convention (l2ui exports L2UI_Init).
// Tools that prefer the legacy "Init" name also find this DLL.

extern "C" __declspec(dllexport) void L2Voice_Init() {
    // No-op. DllMain does the real work; this export exists so the
    // IAT-binder has a symbol to anchor on.
}

extern "C" __declspec(dllexport) void Init() {
    // Alias for tools that expect the generic "Init" name.
}

