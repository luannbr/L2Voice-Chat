# L2Voice-Chat

In-game voice chat for Lineage II private servers. Three channels —
**Proximity** (3D positional), **Party**, **Clan/Ally** — wired into
the L2 client via a side-loaded DLL plus a small Go relay and a Java
bridge for L2J-family game servers.

**🇧🇷 Versão em português:** [README.pt-BR.md](README.pt-BR.md)

---

## ⚠️ Disclaimer

This project is **not affiliated with, endorsed by, or sponsored by
NCSoft**. "Lineage II" is a trademark of NCSoft Corporation.

This software is intended **exclusively for use on private servers**
that you operate or are authorized to participate in. Using it on
official NCSoft servers may violate their Terms of Service. The
authors take no responsibility for accounts, characters, or actions
taken by third parties using this software.

The repository contains **no copyrighted game assets** — UI textures
referenced by the optional `VOICE_L2_THEME` build flag are not
distributed and must be supplied by the user from a client they own.

Use at your own risk.

---

## What it looks like

Pipeline at a glance:

```
┌────────────────────┐         ┌──────────────────────┐         ┌──────────────┐
│  L2 client + DLL   │◄───────►│  voice-server (Go)   │◄───────►│  L2J bridge  │
│  l2voice.dll       │  UDP    │  SFU + spatial mix   │   WS    │  (Java 17)   │
│  (C++17, Win32)    │  audio  │  :17666 udp          │ events  │  Maven mod   │
│                    │  WS     │  :17667 ws           │ + RPC   │              │
│                    │  ctrl   │                      │         │              │
└────────────────────┘         └──────────────────────┘         └──────────────┘
```

- **Client DLL** — captures from WASAPI, runs AEC (Speex DSP) +
  NS (RNNoise) + AGC + HPF, encodes Opus, sends over UDP. ImGui
  overlay for channel/PTT/volume controls.
- **voice-server** — single Go binary. Authoritative routing,
  proximity math, per-channel mixdown. WebSocket control plane;
  UDP audio plane.
- **l2j-bridge** — Maven module attached to a L2J-family game
  server. Resolves player identity from a TCP-port snapshot, fans
  party/clan/ally events into the voice-server, and answers RPC
  whoami/name queries.

The three pieces talk over documented protocols — see
[`docs/protocol.md`](docs/protocol.md) for the wire format.

## Features

- **Three voice channels.** Proximity (positional), Party (closed
  group), Clan/Ally (global). PTT priority: party > clan > ally >
  proximity.
- **Full audio processing chain.** AEC → HPF → NS → AGC. Cuts echo
  during dual-PC setups and keyboard/mouse noise during raids.
- **No client identity protocol.** The DLL doesn't ship a token.
  Identity is resolved server-side by matching the DLL's TCP source
  ports against the GS's accepted sockets — works with any L2J
  fork without server-side hooks beyond the bridge module.
- **Multi-VPS ready.** The bridge can fan events to N voice-servers
  in parallel; clients connect to the nearest one by URL.
- **No GPL anywhere in the runtime.** Everything ships under MIT/BSD
  permissive licenses, including all bundled native dependencies.

## Tech stack

| Component | Library | License |
|-----------|---------|---------|
| Capture/playback | [miniaudio](https://github.com/mackron/miniaudio) | MIT/Unlicense |
| Opus codec | [libopus](https://opus-codec.org/) | BSD |
| Echo cancellation | [Speex DSP](https://github.com/xiph/speexdsp) | BSD |
| Noise suppression | [RNNoise](https://github.com/xiph/rnnoise) (cpuimage MSVC fork) | BSD |
| WebSocket | [IXWebSocket](https://github.com/machinezone/IXWebSocket) | BSD |
| Hooking | [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause |
| Overlay | [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| voice-server | gorilla/websocket, redis/go-redis, stdlib net | MIT/BSD |
| l2j-bridge | Jedis (Redis client) | MIT |

## Quick start

If you just want to get it running locally:

```bash
# 1. Build the voice-server (needs Go 1.22+)
cd voice-service && go mod tidy && go build -o voice-server.exe ./cmd/voice-server
./voice-server.exe -udp :17666 -ws :17667

# 2. Build the DLL (needs VS2022 + CMake 3.20+)
cd client
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
# → client/build/Release/l2voice.dll

# 3. Drop the DLL next to your L2 client + create voice.ini
#    (see docs/USAGE.md for full ini reference)

# 4. Build the L2J bridge JAR (needs JDK 17 + Maven + your server JARs)
cd l2j-bridge && mvn package
```

Full step-by-step guides:

- 🇺🇸 [**docs/BUILDING.md**](docs/BUILDING.md) — compile all three components
- 🇺🇸 [**docs/USAGE.md**](docs/USAGE.md) — install, configure, and operate
- 🇧🇷 [**docs/BUILDING.pt-BR.md**](docs/BUILDING.pt-BR.md) — guia de compilação
- 🇧🇷 [**docs/USAGE.pt-BR.md**](docs/USAGE.pt-BR.md) — guia de uso

## Compatibility

| L2 client | Status |
|-----------|--------|
| Essence 542 SamuraiCrow (EU) | ✅ verified |
| Other Essence builds | ⚠️ likely works — engine offsets may differ |
| Interlude | ⚠️ DLL injects, name capture limited (see notes) |
| Other chronicles | ❌ untested |

| L2J fork | Status |
|----------|--------|
| l2emuproject Essence 542 | ✅ verified |
| Other L2J Essence forks | ⚠️ bridge needs minor API adaptation |
| Mainstream L2J / aCis / etc. | ⚠️ bridge needs L2World API porting |

## Project layout

```
.
├── LICENSE                  MIT
├── README.md                this file (English)
├── README.pt-BR.md          Portuguese version
├── docs/
│   ├── protocol.md          wire format (UDP audio + WS control + RPC)
│   ├── BUILDING.md          build guide (EN)
│   ├── BUILDING.pt-BR.md    guia de compilação
│   ├── USAGE.md             usage guide (EN)
│   ├── USAGE.pt-BR.md       guia de uso
│   └── DESIGN.pt-BR.md      original design brief (Portuguese)
├── client/                  l2voice.dll (C++17, Win32, MSVC)
│   ├── CMakeLists.txt
│   ├── dllmain.cpp
│   └── voice/               capture/playback/codec/net/overlay/apm
├── voice-service/           voice-server (Go 1.22+)
│   ├── cmd/voice-server/
│   └── internal/
└── l2j-bridge/              Maven module (JDK 17)
    └── src/main/java/com/luannbr/l2voice/bridge/
```

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Monorepo + protocol doc | ✅ |
| 2 | Proximity voice in DLL | ✅ |
| 3 | Go voice-service (proximity + groups) | ✅ |
| 4 | L2J bridge (identity, events, RPC) | ✅ |
| 5 | Audio processing chain (AEC + NS + AGC) | ✅ |
| 6 | Clan voice w/ operational modes | ✅ MVP |
| — | Olympiad / siege / multibox edge cases | ⏳ in progress |

## Contributing

Issues and PRs welcome. Before opening a large PR, please open an
issue first to discuss scope. The codebase mixes English and
Portuguese comments — English is preferred for new code.

## License

MIT — see [LICENSE](LICENSE).

Bundled dependencies retain their respective licenses (all permissive:
MIT, BSD, BSD-2-Clause). No GPL code in the runtime.
