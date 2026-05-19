# Voice System — Protocol Specification

**Version:** 1 (draft rev 2 — server-side position pivot)
**Date:** 2026-05-19
**Target:** L2 Essence 542 SamuraiCrow client + Go voice service + L2J bridge

This document defines the wire formats between the three parts of the
system. Once approved, deviation requires a new protocol version
(`u8 version` field).

> **Rev 2 change (2026-05-19):** Player position is now sourced from
> the L2J server (authoritative) instead of read from client memory.
> Client UDP packets no longer carry x/y/z/instance_id. The voice-
> service performs spatial routing using positions pushed by the L2J
> bridge over Redis, and computes per-receiver `gain`+`pan` which it
> stamps into the outbound packet so the client only mixes.

---

## 1. Transports

| Transport | Direction | Purpose |
|-----------|-----------|---------|
| UDP | client ↔ voice-service, both ways | audio frames (Opus payload + minimal header). Low latency, drops acceptable |
| WebSocket (TCP) | client ↔ voice-service | control/signaling, auth, topology updates. Ordered, reliable |
| Redis pub/sub | l2j-bridge → voice-service | player positions + social events |
| HTTP | voice-service → l2j-bridge | token validation; player metadata fetch |

**Endpoints (defaults; configurable):**
- UDP audio: `udp/17666`
- WS control: `tcp/17667` (path: `/ws`)
- L2J bridge HTTP: `tcp/17668`
- Redis: `tcp/6379` (channel `l2voice:events`)

**TLS:**
- UDP audio: **plaintext** (latency-critical; session_id is per-login secret)
- WebSocket: **WSS recommended** in production
- HTTP auth endpoint: **HTTPS recommended** in production

---

## 2. Authentication flow

```
1. Player logs into L2 (existing flow, l2voice.dll already loaded)
2. L2J generates a one-time voice token at OnPlayerLogin:
     token = HMAC(secret_key, player_object_id || login_timestamp)
3. L2J broadcasts the token to the client via a custom system message
   (Say2 type 18) OR a small custom packet l2voice.dll intercepts
4. DLL captures the token, opens WS to voice-service
5. DLL sends `auth` with token + player_object_id
6. Voice-service POSTs to L2J `/voice/validate` with token+player_id
7. L2J validates, returns session_id + player metadata
8. Voice-service responds `auth_ok` with session_id and UDP endpoint
9. DLL begins sending UDP audio with session_id in header
```

Session ends on: WS disconnect (60s idle), explicit `logout`,
L2J `player_logout` event, or `/voice/validate` failure.

---

## 3. UDP audio packet format

All multi-byte fields are **little-endian**. Fields are packed.

The protocol is **asymmetric** because spatial routing happens in the
service:

- **Ingress** (client → service): just announces audio frames. The
  service knows the sender's position from L2J, so the client doesn't
  carry it.
- **Egress** (service → client): adds 2 bytes of pre-computed spatial
  info (`gain`, `pan`) for the proximity channel so the client only
  mixes — no per-frame trigonometry on the client side.

### 3.1 Ingress — client → service (8 bytes header + payload)

```
offset  size  field
------  ----  -------------------------------------------------
0       1     version       protocol version (0x01)
1       1     channel       0=proximity, 1=party, 2=clan, 3=ally,
                            4=siege, 5=keepalive, 6=ping_req
2       2     seq_lo        sequence number low 16 bits
4       4     session_id    assigned by service at auth_ok
8+      ...   opus_payload  Opus frame (omitted for channel 5/6)
```

Total ingress packet size ≤ 8 + 1024 = 1032 bytes.

### 3.2 Egress — service → client

**Proximity (channel 0): 10 bytes header + payload.**

```
offset  size  field
------  ----  -------------------------------------------------
0       1     version          0x01
1       1     channel          0 (proximity)
2       2     seq_lo           passed through from sender
4       4     src_session_id   who is talking
8       1     gain             0..255  (255 = full, 0 = silent;
                                service never sends 0 — drops packet
                                instead)
9       1     pan              int8 -127..+127 (0 = center,
                                negative = left, positive = right)
10+     ...   opus_payload
```

**Party/clan/ally/siege (channels 1..4): 8 bytes header + payload.**

Same as ingress shape, with `src_session_id` in the sid field. No
spatial bytes — these channels are non-positional.

**Ping_resp (channel 7): 8 bytes header only.** Echoes back the
seq_lo of the corresponding ping_req.

### 3.3 Opus frame parameters (fixed)

- Sample rate: 48 000 Hz
- Channels: 1 (mono)
- Frame size: 20 ms (960 samples)
- Bitrate: 24 kbps
- Complexity: 5
- FEC: enabled
- Application: VOIP

Fixed in the protocol — both sides agree without negotiation. Bump
`version` to change.

---

## 4. WebSocket control protocol

UTF-8 JSON, one message per WS frame. Both sides validate `type`.

### 4.1 Client → Server

| `type` | Payload fields |
|--------|----------------|
| `auth` | `token`, `player_id`, `client_version` |
| `channel_join` | `channel` (party/clan/ally) |
| `channel_leave` | `channel` |
| `mute` | `target_player_id`, `muted` |
| `state_update` | `ptt_channel`, `speaking` |
| `logout` | (none) |

Proximity channel is auto-joined at auth.

### 4.2 Server → Client

| `type` | Payload fields |
|--------|----------------|
| `auth_ok` | `session_id`, `udp_endpoint`, `your_player_id` |
| `auth_fail` | `reason` (`token_invalid`, `token_expired`, `player_not_found`, `already_connected`) |
| `topology_update` | `party_members[]`, `clan_members[]`, `ally_members[]`, `instance_id`, `in_olympiad`, `in_siege` |
| `speaker_state` | `player_id`, `channel`, `speaking` |
| `error` | `code`, `message` |

---

## 5. Redis pub/sub (l2j-bridge → voice-service)

Channel: `l2voice:events`
Format: UTF-8 JSON, one message per pub.

```json
{
  "ts": 1716124800000,
  "event": "position",
  "player_id": 1234567,
  "data": { ... }
}
```

### 5.1 Event types

| `event` | When | `data` fields |
|---------|------|----------------|
| `player_login` | OnPlayerLogin | `name`, `class_id`, `level`, `clan_id`, `ally_id`, `party_id`, `x`, `y`, `z`, `instance_id` |
| `player_logout` | OnPlayerLogout / disconnect | (none) |
| `position` | broadcast every ~200ms while moving; once on stop | `x`, `y`, `z`, `instance_id` |
| `party_join` | OnPartyChange | `party_id`, `member_ids[]` |
| `party_leave` | OnPartyChange | `party_id` |
| `clan_change` | OnClanJoin / OnClanLeave | `clan_id` (0 = left) |
| `ally_change` | OnAllyChange | `ally_id` (0 = left) |
| `instance_change` | OnEnterInstance / OnExitInstance | `instance_id` |
| `zone_special` | enter/exit olympiad/siege/peace | `kind` (oly/siege/peace), `entering` (bool) |

### 5.2 Position broadcast cadence

The bridge throttles per player:
- Movement: ≤ 5 Hz (one message per 200 ms), only if `|Δp| > 50 cm`
- Stationary: one final message at stop, then suppressed
- Teleport / instance change: an extra synchronous message

The voice-service maintains `player_id → {x,y,z,instance_id}` in memory
and recomputes per-receiver `gain`/`pan` at the moment it routes each
audio packet. Position freshness window: positions older than 2 s are
treated as "unknown" and the player is dropped from the proximity set
for that frame.

### 5.3 Spatial computation (service-side)

When forwarding a proximity packet from speaker `S` to receiver `R`:

```
dx = S.x - R.x
dy = S.y - R.y
dz = S.z - R.z
dist = sqrt(dx² + dy² + dz²)
if dist >= max_dist_cm:  drop (don't send)
gain_f = 1 - max(0, dist - min_dist_cm) / (max_dist_cm - min_dist_cm)
gain_u8 = round(gain_f * 255)
pan_f = clamp(dx / 1500, -1, +1)        # horizontal-only
pan_i8 = round(pan_f * 127)
```

`min_dist_cm` and `max_dist_cm` are configurable on the service
(defaults 500 / 2500). Clients can override locally for HUD purposes
but the wire decision is service-authoritative.

---

## 6. HTTP — voice-service → l2j-bridge

### `POST /voice/validate`

Request:
```json
{
  "token": "base64url-hmac",
  "player_id": 1234567
}
```

Response 200:
```json
{
  "valid": true,
  "session_id": 305419896,
  "player": {
    "name": "Litch",
    "class_id": 105,
    "level": 85,
    "clan_id": 42,
    "ally_id": 7,
    "party_id": 0,
    "x": 81000, "y": 148000, "z": -3470,
    "instance_id": 0
  }
}
```

Response 401:
```json
{ "valid": false, "reason": "token_expired" }
```

Called once per WS auth. Subsequent UDP packets reuse the cached
session.

---

## 7. Open decisions (resolved with defaults; can revisit)

1. **Token delivery to client:** Say2 system message (simpler v1).
2. **WS encoding:** JSON (easier to debug; low volume).
3. **NAT:** assume outbound UDP works (most home NATs). STUN/TURN if needed.
4. **Server-side mixing vs client-side mixing:** **client mixes**; service routes + stamps gain/pan only.
5. **UDP encryption:** plaintext (session_id is the secret).
