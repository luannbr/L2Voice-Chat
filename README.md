# Voice System for L2 Essence 542

Three-channel voice chat integrated into the L2 client: proximity (3D
positional), party (closed group), clan/ally (global). Ships as its
own DLL — **`l2voice.dll`**, separate from `l2ui.dll` (AutoLogin).
Both can be injected side-by-side via the Engine.dll IAT method.

## Target

- **Client:** L2 Essence 542 SamuraiCrow (EU build) — `engine.dll` TS=0x6928282b
- **Server:** L2J l2emuproject Essence (this source: `Source-GS_SR-542-main`)
- **Voice service:** Go, runs initially on the dev box; deployable to Linux VPS later

## Architecture

```
┌─────────────────────┐         ┌──────────────────────┐         ┌─────────────┐
│  Cliente L2 + DLL   │◄───────►│  Serviço de Voz (Go) │◄───────►│   L2J Core  │
│  (C++, MSVC v143)   │  UDP    │   SFU + Mixer        │  Redis  │  (Java 17)  │
│  l2voice.dll        │  audio  │   :17666 udp         │  pub/sub│  + bridge   │
│  (standalone)       │  WS     │   :17667 ws          │         │  module     │
│                     │  ctrl   │                      │         │  :17668 http│
└─────────────────────┘         └──────────────────────┘         └─────────────┘
```

See `docs/protocol.md` for wire formats.

## Layout

```
Voice_System/
├── Prompt.txt             original brief
├── README.md              this file
├── docs/
│   └── protocol.md        binary UDP + WS JSON + Redis pub/sub spec
├── client/                STANDALONE DLL → l2voice.dll
│   ├── CMakeLists.txt     top-level build
│   ├── dllmain.cpp        entry point + 50 Hz poll thread
│   └── voice/             module: capture, playback, codec, net, memreader
├── voice-service/         Go service (standalone)
│   ├── cmd/voice-server/  main package
│   └── internal/          audio router, topology, auth
└── l2j-bridge/            Java module attached to l2emuproject Essence 542 GS
```

## Tech stack

| Component | Library | Why |
|-----------|---------|-----|
| client / capture/playback | [miniaudio](https://github.com/mackron/miniaudio) | MIT/Unlicense, single header, WASAPI on Win |
| client / codec | [Opus](https://opus-codec.org/) | BSD, low latency, 24 kbps target |
| client / net (UDP audio) | raw `WinSock2` | no deps, our own protocol |
| client / net (WS control) | [IXWebSocket](https://github.com/machinezone/IXWebSocket) or `websocketpp` | BSD/MIT |
| client / UI | ImGui (already present) | already in DLL |
| voice-service | `gorilla/websocket`, `redis/go-redis`, stdlib `net` | MIT/BSD |
| l2j-bridge | adapts to whatever the GS source uses; Redis client `jedis` | BSD |

**No GPL** anywhere — confirmed for distributable build.

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1     | Monorepo structure + protocol doc | ✅ approved |
| 2     | Proximity-only voice module in DLL | in progress |
| 3     | Go voice-service for proximity | in progress |
| 4     | L2J bridge | pending |
| 5     | Special cases (Olympiad/siege/multibox/death) | pending |

Scope reduced to **proximity only** for the first end-to-end demo; party/clan/ally come after proximity works.

## Build requirements

| Component | Tools |
|-----------|-------|
| `client/` | VS2022 + CMake 3.20+ (own project; see `client/README.md`) |
| `voice-service/` | Go 1.22+ — install from https://go.dev/dl/ |
| `l2j-bridge/` | JDK 17 + Maven (matches l2emuproject Essence build) |

Run `voice-service` locally:
```
cd voice-service
go mod tidy
go build -o voice-server.exe ./cmd/voice-server
./voice-server.exe -udp :17666 -ws :17667
```

## How to read this repo before contributing

1. `Prompt.txt` — original brief
2. `docs/protocol.md` — wire formats (read this BEFORE coding either side)
3. `client/README.md` — how to build and inject `l2voice.dll`
4. Existing memories under `~/.claude/projects/.../memory/` — known engine RVAs, injection method (Engine.dll IAT), gotchas
