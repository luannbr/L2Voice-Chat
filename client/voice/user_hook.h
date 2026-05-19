// user_hook.h — MinHook on engine.dll!User::SetName to capture the
// local player's User pointer + auto-discover ObjectId.
//
// Why: l2voice runs as a separate DLL from l2ui (AutoLogin). l2ui has
// its own SetName hook for window-title work; ours is independent.
// We capture the *first* User::SetName after Init() (which fires at
// EnterWorld for the local player) and scan the struct for the
// ObjectId so the WS auth carries the real player_id without any
// voice.ini / env-var help.

#pragma once

#include <cstdint>

namespace voice {

// Install the MinHook on engine.dll!User::SetName for the current
// engine. Returns false if engine.dll isn't loaded yet or if the
// hook fails. Logs progress to OutputDebugString.
//
// on_local_id is called once the local player's ObjectId is
// discovered. Cheap; runs on the engine thread.
using LocalIdCallback = void(*)(uint32_t player_id);
bool InstallUserHook(LocalIdCallback on_local_id);

// Tear down the hook. Safe to call even if Install failed.
void UninstallUserHook();

}  // namespace voice
