// memory_reader.h — reads the local player's transient state from L2's
// in-memory User struct so we can stamp it into outgoing proximity
// packets. Strictly read-only.
//
// The struct offsets are not stable across L2 chronicles / patches.
// Each supported build registers its offsets in client_profiles.h
// (existing); we just consume them here.
//
// We rely on the existing g_localUser pointer captured by l2ui.dll's
// existing hooks (User::SetName etc) — see d3d9_hook.cpp. If the
// pointer is null or the offsets aren't filled for the active
// profile, ReadLocalPlayerXYZInstance returns false.

#pragma once

#include <cstdint>

namespace voice {

struct LocalPlayerState {
    float    x, y, z;      // L2 native units (cm-ish, may be raw float)
    uint32_t instance_id;  // 0 = main world
    bool     ok;           // false if we couldn't read this frame
};

// Atomically copies the latest cached state. Cheap; called once per
// outgoing audio frame (50 Hz).
LocalPlayerState ReadLocalPlayerState();

// Internal: called from the existing memory-poll site in
// d3d9_hook.cpp to refresh the cache. Walks the User struct at the
// configured offsets, validates with SEH wrappers.
void RefreshLocalPlayerState();

}  // namespace voice
