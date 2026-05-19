// memory_reader.cpp — reads local player's transient state.
//
// MVP status: STUB. Until we identify the User struct offsets for
// X/Y/Z and instance_id on Essence 542 (client_profiles.h does not
// expose them yet), ReadLocalPlayerState() returns ok=false. The
// voice pipeline tolerates that by sending zeroed coordinates — the
// service still routes, and the loopback test path doesn't need real
// position.
//
// Refresh is called once per frame from RefreshLocalPlayerState() in
// the d3d9_hook render loop. The reads are guarded by __try/__except
// so an offset miss can't crash the client.

#include "memory_reader.h"

#include <windows.h>
#include <atomic>

namespace voice {

namespace {

struct AtomicState {
    std::atomic<uint32_t> seq{0};
    LocalPlayerState pending{};
    LocalPlayerState visible{};
};

AtomicState g_state;

// Placeholders. When the offsets are discovered, set g_localUser
// from the existing l2ui.dll capture site and fill these.
//
// Example shape (NOT real for Essence 542 yet):
//   constexpr ptrdiff_t kOffX        = 0x40;
//   constexpr ptrdiff_t kOffY        = 0x44;
//   constexpr ptrdiff_t kOffZ        = 0x48;
//   constexpr ptrdiff_t kOffInstance = 0x6C;
constexpr ptrdiff_t kOffX = 0;
constexpr ptrdiff_t kOffY = 0;
constexpr ptrdiff_t kOffZ = 0;
constexpr ptrdiff_t kOffInstance = 0;

void* GetLocalUserPtr() {
    // l2voice is its own DLL (NOT coupled to l2ui), so it must
    // capture the local User* via its own hook. Candidate hook
    // points (same techniques l2ui uses, applied independently):
    //   - User::SetName  (UScript native or RVA)
    //   - GRegisterNative tap (see memory: l2ui-gregisternative-technique)
    // Until that hook lands here, we return null → stub mode.
    return nullptr;
}

bool SafeReadFloat(void* base, ptrdiff_t off, float& out) {
    __try {
        out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU32(void* base, ptrdiff_t off, uint32_t& out) {
    __try {
        out = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void RefreshLocalPlayerState() {
    LocalPlayerState s{};
    void* user = GetLocalUserPtr();
    if (!user || kOffX == 0) {
        // Stub mode — coordinates unknown.
        s.ok = false;
        g_state.pending = s;
        g_state.seq.fetch_add(1, std::memory_order_release);
        g_state.visible = s;
        return;
    }
    bool ok = true;
    ok &= SafeReadFloat(user, kOffX, s.x);
    ok &= SafeReadFloat(user, kOffY, s.y);
    ok &= SafeReadFloat(user, kOffZ, s.z);
    ok &= SafeReadU32  (user, kOffInstance, s.instance_id);
    s.ok = ok;
    g_state.pending = s;
    g_state.seq.fetch_add(1, std::memory_order_release);
    g_state.visible = s;
}

LocalPlayerState ReadLocalPlayerState() {
    // Snapshot via seq guard. Producer is single-threaded (render
    // loop) so this is a cheap correctness check rather than a hot
    // synchronization point.
    for (int spin = 0; spin < 4; ++spin) {
        uint32_t s1 = g_state.seq.load(std::memory_order_acquire);
        LocalPlayerState v = g_state.visible;
        uint32_t s2 = g_state.seq.load(std::memory_order_acquire);
        if (s1 == s2) return v;
    }
    return LocalPlayerState{};
}

}  // namespace voice
