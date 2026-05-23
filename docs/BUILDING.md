# Building L2Voice-Chat

Step-by-step build for all three components: the Windows DLL, the Go
voice-server, and the L2J Java bridge.

> 🇧🇷 Versão em português: [BUILDING.pt-BR.md](BUILDING.pt-BR.md)

---

## Prerequisites

| Component | Tools | Tested versions |
|-----------|-------|-----------------|
| `client/` (DLL) | Visual Studio 2022 + C++ Desktop workload, CMake ≥ 3.20, Git | VS 17.10, CMake 3.29 |
| `voice-service/` | Go ≥ 1.22 | Go 1.22.x |
| `l2j-bridge/` | JDK 17, Maven 3.9+, your server's GameServer JAR | OpenJDK 17.0.x, Maven 3.9.6 |

Get the tools:

- Visual Studio 2022 Community → <https://visualstudio.microsoft.com/downloads/> (select **Desktop development with C++**)
- CMake → <https://cmake.org/download/>
- Go → <https://go.dev/dl/>
- JDK 17 → <https://adoptium.net/> (Temurin) or any OpenJDK 17 distro
- Maven → <https://maven.apache.org/download.cgi> (or via `choco install maven`)

Internet access is required for the first `cmake` configure — it
fetches Opus, miniaudio, RNNoise, Speex DSP, MinHook, IXWebSocket,
and Dear ImGui via FetchContent (~5 minutes on a clean checkout,
cached afterwards in `client/build/_deps/`).

---

## 1. Build the client DLL (`l2voice.dll`)

The L2 client is **32-bit**, so the DLL must be built for **Win32**
(the CMakeLists will refuse otherwise).

```bat
git clone https://github.com/luannbr/L2Voice-Chat.git
cd L2Voice-Chat\client
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Output: `client\build\Release\l2voice.dll` (~1.5 MB).

### Optional: L2 native theme

The overlay can use textures extracted from a Lineage II client for a
native look. These textures are **NCSoft copyrighted** and are NOT
distributed with this repo. To enable the L2 theme:

1. Copy the L2UI_CH3 PNGs from a client you own into a
   `l2ui_assets/` folder at the repository root.
2. Re-run cmake with `-DVOICE_L2_THEME=ON`.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DVOICE_L2_THEME=ON
cmake --build build --config Release
```

If the flag is OFF (default), the overlay falls back to the standard
ImGui look. This is the supported open-source path.

### Common build errors

| Symptom | Fix |
|---------|-----|
| `cmake: must be built for Win32 (32-bit)` | Add `-A Win32` to your configure command. |
| `Cannot find Visual Studio 17 2022` | Install the **Desktop development with C++** workload. |
| Long FetchContent hang at first configure | Allow ~5 min on first run; subsequent configures are cached. |
| RC compiler fails on `l2ui_assets/...` | You enabled `VOICE_L2_THEME` but didn't supply the textures. Disable the flag or supply them. |

---

## 2. Build the voice-server (Go)

```bash
cd voice-service
go mod tidy
go build -o voice-server.exe ./cmd/voice-server   # Windows
# or
go build -o voice-server ./cmd/voice-server       # Linux/macOS
```

Output: `voice-service/voice-server[.exe]` (~12 MB static binary, no
runtime deps).

### Smoke test (no L2J)

```bash
./voice-server.exe -udp :17666 -ws :17667
```

You should see:

```
voice-service starting (udp=:17666 ws=:17667)
WS listener ready on :17667
UDP listener ready on :17666
```

The server is now ready to accept DLL connections and bridge events.

### Useful flags

| Flag | Purpose |
|------|---------|
| `-udp :17666` | UDP listener for Opus audio packets |
| `-ws :17667` | WS listener (control + bridge) |
| `-redis 127.0.0.1:6379` | Optional Redis bus for legacy events (not required if using the bridge WS path) |
| `-multibox-mute=false` | Disable same-IP mute (useful for self-test with multiple clients on one machine) |

---

## 3. Build the L2J bridge (JAR)

The bridge is a Maven module that plugs into a L2J game server. You
**need the GameServer JAR from your server distribution** — it's a
build dependency (the bridge references L2J classes like
`L2PcInstance`, `L2World`, etc.).

### 3.1. Install your server's GameServer JAR into your local Maven repo

If your fork is `l2emuproject Essence 542` and its built JAR is at
`H:\path\to\gameserver.jar`:

```bash
mvn install:install-file ^
    -Dfile="H:\path\to\gameserver.jar" ^
    -DgroupId=com.l2emuproject ^
    -DartifactId=gameserver ^
    -Dversion=542 ^
    -Dpackaging=jar
```

The exact `groupId` / `artifactId` / `version` must match
`l2j-bridge/pom.xml`. Default values target `l2emuproject Essence 542`
— adjust the pom if your fork differs.

### 3.2. Build the bridge

```bash
cd l2j-bridge
mvn package
```

Output: `l2j-bridge/target/l2voice-bridge-0.1.0.jar`.

### 3.3. Deploy to the GS

Copy `l2voice-bridge-0.1.0.jar` into your GS's `libs/` (or
equivalent) directory, then add this to your GS's startup config:

```properties
# gameserver/config/l2voice.properties
l2voice.voice_server.urls = ws://127.0.0.1:17667/bridge
l2voice.enabled           = true
```

For multi-VPS routing, use a comma-separated list:

```properties
l2voice.voice_server.urls = ws://br.example.com:17667/bridge,ws://us.example.com:17667/bridge
```

Restart the GS. You should see in the GS log:

```
[VoiceBridge] 1 voice-server link(s) started
[VoiceBridge] voice-link connected to ws://127.0.0.1:17667/bridge
```

---

## Building everything in one shot (PowerShell)

```powershell
# From the repo root
$ErrorActionPreference = "Stop"
cmake -S client -B client/build -G "Visual Studio 17 2022" -A Win32
cmake --build client/build --config Release

cd voice-service
go mod tidy
go build -o voice-server.exe ./cmd/voice-server
cd ..

cd l2j-bridge
mvn package
cd ..
```

End-to-end build time on a modern machine: ~6 minutes (most of that
is the first-ever FetchContent of native deps).

---

## Where to go next

- 📖 [USAGE.md](USAGE.md) — install, configure, and operate the system
- 📖 [protocol.md](protocol.md) — wire format reference (for contributors)
- 📖 [DESIGN.pt-BR.md](DESIGN.pt-BR.md) — original design brief
