# Using L2Voice-Chat

Once you've built the three components ([BUILDING.md](BUILDING.md)),
this guide walks through installing, configuring, and using the
system end-to-end.

> 🇧🇷 Versão em português: [USAGE.pt-BR.md](USAGE.pt-BR.md)

---

## Architecture recap

```
   ┌──────────────────────┐                ┌─────────────────────┐
   │  L2 Client + l2voice │ ── UDP audio ─►│                     │
   │  (Win32 DLL)         │ ◄─── WS ctrl ──┤   voice-server      │
   └──────────────────────┘                │   (Go binary)       │
              ▲                            │                     │
              │ TCP source-port                                  │
              │ matching                   └─────────────────────┘
              ▼                                        ▲
   ┌──────────────────────┐                            │
   │   L2J Game Server    │ ── events/RPC via WS ──────┘
   │   + l2voice-bridge   │
   └──────────────────────┘
```

Three boxes to set up: GS + bridge JAR, voice-server, and the L2
client with the DLL.

---

## 1. Install the voice-server

The voice-server is a single binary with **no runtime dependencies**.
Place it wherever it's convenient — most users run it on the same
box as the GS for the first deployment, then move it to a low-ping
VPS later.

### Run it

```bash
./voice-server.exe -udp :17666 -ws :17667
```

For unattended operation, wrap it in `nssm` (Windows service) or a
systemd unit (Linux). Example minimal `start-voice-server.cmd`:

```bat
@echo off
voice-server.exe -udp :17666 -ws :17667 > server.log 2>&1
```

### Firewall

Open ports `17666/udp` and `17667/tcp` on the voice-server host. On
Windows:

```powershell
New-NetFirewallRule -DisplayName "voice-server UDP" -Direction Inbound -Protocol UDP -LocalPort 17666 -Action Allow
New-NetFirewallRule -DisplayName "voice-server WS"  -Direction Inbound -Protocol TCP -LocalPort 17667 -Action Allow
```

---

## 2. Set up the L2J bridge

You've already built and copied `l2voice-bridge-0.1.0.jar` to the GS
in [BUILDING.md §3.3](BUILDING.md). Now configure it:

`gameserver/config/l2voice.properties`:

```properties
# Comma-separated list of voice-server endpoints. The bridge will
# connect to each one and fan events out to all of them. Players
# auto-select the nearest one via ws_url in their voice.ini.
l2voice.voice_server.urls = ws://127.0.0.1:17667/bridge

# Master switch. Set to false to disable the voice system entirely
# without removing the JAR.
l2voice.enabled = true

# Optional HMAC secret used for legacy token-based auth. Not needed
# with the default TCP-port identity resolution.
l2voice.hmac.secret = REPLACE-WITH-A-LONG-RANDOM-STRING
```

Restart the GS. Confirm in the log:

```
[VoiceBridge] 1 voice-server link(s) started
[VoiceBridge] voice-link connected to ws://127.0.0.1:17667/bridge
[VoiceBridge] voice-link: rtt=2ms
```

If you see `voice-link disconnected (will retry)` instead, the
voice-server isn't reachable — check firewall and that the
voice-server is running.

---

## 3. Install the client DLL

The DLL is **side-loaded** into the L2 client via the Engine.dll IAT
hijack method. This avoids modifying `L2.exe` (which Themida
protects).

### Step-by-step

1. Copy `l2voice.dll` into your L2 client directory (next to
   `L2.exe` and `Engine.dll`).
2. Make sure your Engine.dll IAT injector loads `l2voice.dll`. If
   you already use a custom DLL loader (e.g. for `l2ui.dll` /
   AutoLogin), add `l2voice.dll` to its list. Both DLLs can coexist.
3. Create `voice.ini` next to the DLL (next section).

> ⚠️ **Do not modify `L2.exe`**. Themida corrupts D3D9 device
> recreate on a CFF-modified L2.exe. The Engine.dll IAT method
> bypasses this entirely.

### `voice.ini` — full reference

```ini
[voice]
# WebSocket URL of the voice-server. Use the public IP/host if the
# voice-server is on a VPS.
ws_url = ws://127.0.0.1:17667/ws

# Master enable/disable. Set to 0 to keep the DLL loaded but mute.
enabled = 1

# Auto-connect on attach. If 0, must call voice::Init() manually.
auto_connect = 1

# Require L2 to have foreground focus before transmitting (prevents
# accidental TX when alt-tabbed).
require_focus = 1

# Push-to-talk virtual key codes. Defaults match VK_H/B/N/M for
# proximity/party/clan/ally — see Microsoft's VK_* table.
ptt_proximity = 72   ; VK_H
ptt_party     = 66   ; VK_B
ptt_clan      = 78   ; VK_N
ptt_ally      = 77   ; VK_M

# Always-on transmit on proximity (no PTT needed). Use with caution.
always_on = 0

# Proximity falloff (centimeters in L2 world units).
min_dist_cm = 500
max_dist_cm = 2500

# Master volume 0..200 (= 0.0..2.0). 100 = unity.
master_volume = 100

# Per-channel preferences (0=disabled, 1=enabled).
ch_enabled_0 = 1   ; proximity
ch_enabled_1 = 1   ; party
ch_enabled_2 = 1   ; clan
ch_enabled_3 = 1   ; ally

# Per-channel volume 0..200.
ch_volume_0 = 100
ch_volume_1 = 100
ch_volume_2 = 100
ch_volume_3 = 100

# Active TX channel for the main PTT (and always_on).
# 0=Proximity, 1=Party, 2=Clan, 3=Ally, 4=CC.
active_tx_channel = 0

# APM (audio processing module). All on = best quality.
apm_aec = 1   ; Speex DSP echo canceller
apm_hpf = 1   ; 80 Hz high-pass
apm_ns  = 1   ; RNNoise neural noise suppression
apm_agc = 1   ; auto gain control
```

---

## 4. First voice call

1. Start the voice-server.
2. Start the GS (with the bridge configured).
3. Launch the L2 client (with DLL injected) and enter the world.
4. Open the overlay — the in-game panel appears automatically when
   the DLL detects the L2 window has focus.
5. Verify the overlay shows `connected` (green dot) and a session ID.
6. Hold the **proximity PTT key** (default `H`) and speak. Anyone
   near you (within `max_dist_cm`) on the same instanceId hears you.
7. For group voice: hold the **party/clan/ally PTT key** (default
   `B/N/M`) to talk on that channel.

### Overlay controls

- **Channel tabs** — toggle channel enable, adjust per-channel volume
- **Speaker list** — shows currently transmitting players, with per-source mute and volume sliders
- **Settings** — master volume, PTT remap, always-on toggle

### Indicators

| Icon | Meaning |
|------|---------|
| 🎤 (gold) | Idle — capturing audio but not transmitting |
| 🎤 (red) | Transmitting (PTT held or always_on) |
| 🔇 | Capture device unavailable or blocked |

---

## 5. Multi-VPS deployment

For minimum ping per region, run one voice-server per region and
list all of them in the bridge config. The bridge fans events to
all voice-servers; each client connects to whichever URL its
`voice.ini` points to.

Example:

```
gameserver/config/l2voice.properties:
  l2voice.voice_server.urls = ws://br.voice.example.com:17667/bridge,ws://us.voice.example.com:17667/bridge

client's voice.ini (BR player):
  ws_url = ws://br.voice.example.com:17667/ws

client's voice.ini (US player):
  ws_url = ws://us.voice.example.com:17667/ws
```

Players on different voice-servers **do not currently hear each
other** — federation between voice-servers is on the roadmap.
Today, BR↔US works only if both are on the same voice-server.

---

## 6. Troubleshooting

### The overlay never appears

- Confirm `l2voice.dll` is in the client folder.
- Confirm your IAT injector is loading it (check Process Monitor for
  the DLL load event when launching L2).
- Check the Visual Studio debug output (DebugView++) for
  `[l2voice] ...` lines.

### Overlay shows `disconnected`

- Voice-server isn't running, or `ws_url` in `voice.ini` is wrong.
- Firewall blocking port 17667.

### Auth never completes (overlay shows `connected` but `player_id=0`)

- The bridge isn't talking to the voice-server.
- Confirm the GS log shows `voice-link connected`.
- If the GS and the L2 client are on the same machine, this used to
  fail due to a loopback IP mismatch — fixed in current versions
  (the bridge now accepts loopback host with port-only match).

### You can hear yourself in your other clients (multibox)

- Voice-server enforces same-IP mute by default. To test with
  multiple clients on the same PC, restart with `-multibox-mute=false`.
- For production, keep same-IP mute on — it prevents the multibox
  feedback loop.

### Echo in your call (other side hears their own voice back)

- The capture device is picking up speakers (no headphones).
- AEC should handle this — verify `apm_aec = 1` in voice.ini.
- Heavy reverb or volume > ~70% can exceed Speex's 100ms tail. Try
  lower output volume or use headphones.

### Static / dropouts during raids

- High packet loss on the UDP path. Check
  `voice-service` log for `dropped frames` warnings.
- Mainstream cause is upstream bandwidth saturation. Voice uses
  ~25 kbps per active speaker — usually negligible unless the user's
  uplink is saturated by something else.

---

## Where to go next

- 📖 [BUILDING.md](BUILDING.md) — build from source
- 📖 [protocol.md](protocol.md) — wire format reference
- 📖 [DESIGN.pt-BR.md](DESIGN.pt-BR.md) — original design brief
