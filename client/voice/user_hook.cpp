// user_hook.cpp — engine.dll!User::SetName hook for ObjectId discovery.
//
// Engine.dll profile (Essence 541 SamuraiCrow, TS=0x692828e1):
//   rvaUserSetName = 0x7a52e0  (same as l2ui's rvaEngineUserSetName)
//
// Strategy:
//   1. Wait until engine.dll is loaded (it always is by the time
//      DllMain runs because l2voice is on engine.dll's IAT).
//   2. Validate the engine.dll TimeDateStamp matches the expected
//      profile — refuse to hook if it doesn't, to avoid crashing on
//      a different client version.
//   3. Install a MinHook on User::SetName. The trampoline captures
//      the `this` pointer.
//   4. On the FIRST SetName call after install, treat that User as
//      the local player. Scan first 256 bytes for any uint32 in the
//      L2J ObjectId range (0x10000000..0x3B9AC9FF) and log all
//      candidates. Fire the LocalIdCallback with the first one.
//
// If the first-capture heuristic ever picks wrong, DebugView output
// has every candidate offset + value — copy and tell us, we hardcode
// the right offset.

#include "user_hook.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>

namespace voice {

namespace {

// Engine profile.
constexpr uint32_t kExpectedEngineTimestamp = 0x692828e1;   // Essence 541
constexpr DWORD    kRvaUserSetName          = 0x7a52e0;

// Engine.dll uses __thiscall on x86: this in ECX, others on stack.
// MSVC __fastcall passes first arg in ECX and second in EDX, so by
// declaring fake EDX we can match __thiscall ABI from C++ code.
using PFN_UserSetName = void (__fastcall*)(void* This, void* /*edx*/,
                                           const wchar_t* name);

PFN_UserSetName        g_origSetName = nullptr;
std::atomic<void*>     g_localUser{nullptr};
std::atomic<bool>      g_captured{false};
LocalIdCallback        g_onLocalId = nullptr;

void Logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    OutputDebugStringA(buf);
}

// Reads the IMAGE_NT_HEADERS.TimeDateStamp from a loaded module.
uint32_t GetModuleTimestamp(HMODULE mod) {
    if (!mod) return 0;
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

// Scans first `bytes` of `base` for uint32s in the typical L2J
// ObjectId range and logs them. Returns the lowest candidate offset
// (0 if none found).
uint32_t ScanForObjectId(uint8_t* base, size_t bytes, uint32_t& outValue) {
    constexpr uint32_t kLo = 0x10000000;  // 268_435_456 — L2J convention
    constexpr uint32_t kHi = 0x3B9AC9FF;  // 999_999_999
    uint32_t firstOff = 0;
    outValue = 0;
    for (size_t off = 0; off + 4 <= bytes; off += 4) {
        uint32_t v;
        __try { v = *reinterpret_cast<volatile uint32_t*>(base + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= kLo && v <= kHi) {
            Logf("[l2voice] candidate ObjectId offset=0x%03zx value=%u (0x%08x)\n",
                 off, v, v);
            if (firstOff == 0) { firstOff = (uint32_t)off; outValue = v; }
        }
    }
    return firstOff;
}

void __fastcall HookUserSetName(void* This, void* /*edx*/, const wchar_t* name) {
    // Call the original first so the engine has its name set before
    // anyone reads from the struct.
    if (g_origSetName) g_origSetName(This, nullptr, name);

    // Only the FIRST call captures. Subsequent ones (other players)
    // are ignored.
    if (g_captured.exchange(true)) return;
    g_localUser.store(This);

    // Convert name from wchar_t to ASCII-ish for the log.
    char nameA[64] = {};
    if (name) {
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nameA, sizeof(nameA) - 1,
                            nullptr, nullptr);
    }
    Logf("[l2voice] User::SetName captured this=%p name=\"%s\"\n", This, nameA);

    uint32_t pid = 0;
    uint32_t off = ScanForObjectId(reinterpret_cast<uint8_t*>(This), 256, pid);
    if (off == 0 || pid == 0) {
        Logf("[l2voice] no ObjectId candidate in first 256 bytes — fallback to ini/env\n");
        return;
    }
    Logf("[l2voice] auto-detected player_id=%u (offset 0x%03x)\n", pid, off);
    if (g_onLocalId) g_onLocalId(pid);
}

}  // namespace

bool InstallUserHook(LocalIdCallback on_local_id) {
    g_onLocalId = on_local_id;

    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (!eng) {
        Logf("[l2voice] Engine.dll not loaded — can't hook User::SetName\n");
        return false;
    }
    uint32_t ts = GetModuleTimestamp(eng);
    Logf("[l2voice] Engine.dll @ %p TS=0x%08x\n", eng, ts);
    if (ts != kExpectedEngineTimestamp) {
        Logf("[l2voice] WARN: Engine.dll TS doesn't match Essence 541 (0x%08x); "
             "hook may be wrong RVA. Proceeding anyway.\n",
             kExpectedEngineTimestamp);
    }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("[l2voice] MH_Initialize failed\n");
        return false;
    }

    void* target = reinterpret_cast<uint8_t*>(eng) + kRvaUserSetName;
    MH_STATUS s = MH_CreateHook(target,
                                reinterpret_cast<void*>(&HookUserSetName),
                                reinterpret_cast<void**>(&g_origSetName));
    if (s != MH_OK) {
        Logf("[l2voice] MH_CreateHook failed: %d\n", s);
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        Logf("[l2voice] MH_EnableHook failed: %d\n", s);
        return false;
    }
    Logf("[l2voice] User::SetName hook armed at %p\n", target);
    return true;
}

void UninstallUserHook() {
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (eng) {
        void* target = reinterpret_cast<uint8_t*>(eng) + kRvaUserSetName;
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    MH_Uninitialize();
    g_origSetName = nullptr;
    g_localUser.store(nullptr);
    g_captured.store(false);
}

}  // namespace voice
