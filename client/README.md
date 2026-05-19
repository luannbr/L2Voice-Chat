# l2voice client (`l2voice.dll`)

Standalone voice client. **Separate from `l2ui.dll` (AutoLogin)** —
both DLLs can coexist in the L2 client directory and are injected
independently.

## Build

Prereqs: Visual Studio 2022 with C++ Desktop workload, CMake 3.20+,
Git.

```bat
cd client
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Output: `client\build\Release\l2voice.dll`.

The first configure pulls libopus, miniaudio and IXWebSocket via
FetchContent (~5 min on a fresh checkout, cached afterwards).

## Injection

Use the same Engine.dll IAT method as `l2ui.dll`. **Do not touch
L2.exe** — Themida will corrupt D3D9 device recreate (see
`project-l2-interlude-devicelost-bug`).

Drop `l2voice.dll` next to the L2 client and add it to whatever
Engine.dll IAT hijack you already use. They're orthogonal DLLs:
l2voice doesn't depend on l2ui being loaded.

## Configuration

Create `voice.ini` next to `l2voice.dll`:

```ini
[voice]
enabled = 1
auto_connect = 1
ws_url = ws://127.0.0.1:17667/ws
min_dist_cm = 500
max_dist_cm = 2500
ptt_proximity = 86   ; VK_V
```

If `voice.ini` is missing, defaults from `voice::DefaultConfig()` apply.

## Loopback test

1. `cd ..\voice-service && go run ./cmd/voice-server -echo`
2. Launch L2 with `l2voice.dll` injected.
3. The DLL auto-connects on attach. Auth token is currently set
   manually — until the L2J bridge (Phase 4) hands one off, you can
   patch a hardcoded test token into `dllmain.cpp::InitWorker` or
   call `voice::SetAuthToken(...)` from an attached debugger.
4. Hold `V` and speak — your own voice should play back through the
   default output device after ~50–100 ms RTT.

## Memory reader status

`memory_reader.cpp` is in **stub mode** until X/Y/Z and instance_id
offsets are identified in the Essence 542 User struct. While stubbed,
outgoing proximity packets carry zeroed coordinates — fine for the
loopback echo test, **not** fine for multi-client spatial routing.

## Layout

```
client/
├── CMakeLists.txt          # top-level build for l2voice.dll
├── dllmain.cpp             # entry: starts a 50 Hz poll thread
└── voice/
    ├── voice.{h,cpp}       # orchestrator + Config + Init/Shutdown
    ├── audio_io.{h,cpp}    # miniaudio capture + 3D mixer playback
    ├── opus_codec.{h,cpp}  # libopus VOIP wrappers
    ├── voice_network.{h,cpp}  # Winsock UDP + IXWebSocket
    └── memory_reader.{h,cpp}  # local-player state (stub)
```
