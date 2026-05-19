# Voice System — Protocol Specification

**Version:** 1 (draft, awaiting approval)
**Date:** 2026-05-19
**Target:** L2 Essence 542 SamuraiCrow client + Go voice service + L2J bridge

This document defines the wire formats between the three parts of the
system. Once approved, deviation requires a new protocol version
(`u8 version` field).

---

## 1. Transports

| Transport | Direction | Purpose |
|-----------|-----------|---------|
| UDP | client ↔ voice-service, both ways | audio frames (Opus payload + metadata). low latency, drops acceptable |
| WebSocket (TCP) | client ↔ voice-service | control/signaling, auth, topology updates. ordered, reliable |
| Redis pub/sub | l2j-bridge → voice-service | server-side social events (party joins, clan changes, zone transitions) |
| HTTP | client → l2j-bridge | one-shot token issuance (after game login) |

**Endpoints (defaults; configurable):**
- UDP audio: `udp/17666`
- WS control: `tcp/17667` (path: `/ws`)
- L2J bridge HTTP: `tcp/17668`
- Redis: `tcp/6379` (channel `l2voice:events`)

**TLS:**
- UDP audio: **plaintext** (latency-critical, content is voice with token-bound session IDs — eavesdropping isn't catastrophic)
- WebSocket: **WSS recommended** in production (TLS, optional in dev — flag `voice.tls=true`)
- HTTP auth endpoint: **HTTPS recommended** for production

---

## 2. Authentication flow

```
1. Player logs into L2 (existing flow, our DLL already loaded)
2. L2J generates a one-time voice token at OnPlayerLogin:
     token = HMAC(secret_key, player_object_id + login_timestamp)
3. L2J broadcasts the token to the client via a custom Say2 system
   message OR a small custom packet our DLL intercepts (TBD — option A
   is simpler for v1, option B is cleaner long-term)
4. DLL captures the token, then opens WS connection to voice-service
5. DLL sends `auth` event with token + player_object_id
6. Voice-service forwards token+player_id to L2J HTTP endpoint
7. L2J validates token, returns session_id (UUID) + player metadata
8. Voice-service responds `auth_ok` with session_id and UDP endpoint
9. DLL begins sending UDP audio with this session_id in header
```

Session expires on:
- WebSocket disconnect (idle timeout 60s)
- Explicit `logout` event
- Player leaves game (L2J publishes event → voice-service kicks)

---

## 3. UDP audio packet format

All multi-byte fields are **little-endian** (x86 native; both client and
service are LE). Fields are packed (no padding).

### 3.1 Common header (8 bytes)

```
offset  size  field        description
------  ----  -----------  -------------------------------------
0       1     version      protocol version (current: 0x01)
1       1     channel      0=proximity, 1=party, 2=clan, 3=ally,
                           4=siege, 5=keepalive, 6=ping_req, 7=ping_resp
2       2     seq_lo       sequence number low 16 bits (high 16 in seq_hi)
4       4     session_id   little-endian uint32, issued by voice-service
                           at auth_ok. Replaces raw player_id on the wire
                           (player_id is mapped by service internally).
```

### 3.2 Channel-specific extensions

**Channel 0 (proximity)** — 20 bytes positional metadata after common header:

```
offset  size  field
------  ----  ------------
8       4     x (f32)         world x in cm (L2 native unit)
12      4     y (f32)         world y
16      4     z (f32)         world z
20      4     instance_id     u32, 0 = main world, non-zero = instance
24      2     seq_hi          sequence number high 16 bits
26      2     opus_len        length of Opus payload (max 1024)
28+     ...   opus_payload    Opus-encoded audio frame (20ms @ 48kHz mono)
```

Total packet size ≤ 28 + 1024 = 1052 bytes (fits well under any MTU).

**Channels 1,2,3,4 (party/clan/ally/siege)** — no positional data:

```
offset  size  field
------  ----  ------------
8       2     seq_hi          sequence number high 16 bits
10      2     opus_len        length of Opus payload
12+     ...   opus_payload    Opus-encoded audio frame
```

Total packet size ≤ 12 + 1024 = 1036 bytes.

**Channel 5 (keepalive)** — header only, no payload. Sent every 15s
when client is silent, to keep NAT mapping open.

**Channel 6/7 (ping_req/resp)** — header only. Used by client to
measure RTT. Service echoes received `seq` back as channel 7 with the
same session_id. Client computes RTT from local timestamp delta.

### 3.3 Opus frame parameters (fixed)

- Sample rate: 48000 Hz
- Channels: 1 (mono)
- Frame size: 20ms (960 samples)
- Bitrate: 24 kbps
- Complexity: 5
- FEC: enabled (Forward Error Correction)
- Application: VOIP

These are fixed in the protocol — both sides agree without negotiation.
If we need to change, bump `version` field.

---

## 4. WebSocket control protocol

UTF-8 JSON, one message per WS frame. Both sides validate `type`.

### 4.1 Client → Server

#### `auth`
```json
{
  "type": "auth",
  "token": "base64url-hmac-from-l2j",
  "player_id": 1234567,
  "client_version": "l2ui/0.5.0"
}
```

#### `channel_join`
```json
{
  "type": "channel_join",
  "channel": "proximity"
}
```
For proximity, joining is automatic at auth; client sends explicit for party/clan/ally.

#### `channel_leave`
```json
{
  "type": "channel_leave",
  "channel": "party"
}
```

#### `mute`
```json
{
  "type": "mute",
  "target_player_id": 999,
  "muted": true
}
```
Client-side mute is enforced at playback; server is informed for UI sync.

#### `state_update`
```json
{
  "type": "state_update",
  "ptt_channel": "proximity",
  "speaking": true
}
```
Sent on PTT key press/release. Server forwards to receivers as
`speaker_state` so they can show "X is speaking" in UI.

#### `logout`
```json
{
  "type": "logout"
}
```
Graceful close. Server tears down session.

### 4.2 Server → Client

#### `auth_ok`
```json
{
  "type": "auth_ok",
  "session_id": 305419896,
  "udp_endpoint": "voice.example.com:17666",
  "your_player_id": 1234567
}
```

#### `auth_fail`
```json
{
  "type": "auth_fail",
  "reason": "token_expired"
}
```
Reasons: `token_invalid`, `token_expired`, `player_not_found`, `already_connected`.

#### `topology_update`
```json
{
  "type": "topology_update",
  "party_members":  [123, 456],
  "clan_members":   [123, 456, 789],
  "ally_members":   [...],
  "instance_id":    0,
  "in_olympiad":    false,
  "in_siege":       0
}
```
Sent on any social change (party/clan/zone). Client uses to draw UI
indicators and to filter incoming audio if local-side spatial wants
to know who's in its party.

#### `speaker_state`
```json
{
  "type": "speaker_state",
  "player_id": 789,
  "channel": "party",
  "speaking": true
}
```
For UI ("X is speaking" indicator).

#### `error`
```json
{
  "type": "error",
  "code": 1001,
  "message": "human-readable"
}
```

---

## 5. Redis pub/sub (l2j-bridge → voice-service)

Channel name: `l2voice:events`
Format: UTF-8 JSON, one message per pub.

```json
{
  "ts": 1716124800000,
  "event": "party_join",   // see list below
  "player_id": 123,
  "data": { ... }          // event-specific
}
```

**Event types:**
| event | data fields |
|-------|------------|
| `player_login` | `name`, `class_id`, `level`, `clan_id`, `ally_id`, `party_id` |
| `player_logout` | (none) |
| `party_join` | `party_id`, `member_ids[]` |
| `party_leave` | `party_id` |
| `clan_join` | `clan_id` |
| `clan_leave` | (none) |
| `ally_change` | `ally_id` (0 = left) |
| `zone_enter` | `zone_type` (oly/siege/etc), `zone_id`, `instance_id` |
| `zone_exit` | `zone_type`, `zone_id` |
| `death` | (none) |
| `revive` | (none) |
| `position` | `x`, `y`, `z`, `instance_id` (low-frequency, only on zone change — high-freq comes via UDP from client) |

Voice-service maintains in-memory topology and reacts. On `party_join`,
all members get a `topology_update` over their WS.

---

## 6. HTTP auth endpoint (l2j-bridge)

`POST /voice/validate`

Request body (JSON):
```json
{
  "token": "...",
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
    "ally_id": 7
  }
}
```

Response 401:
```json
{
  "valid": false,
  "reason": "token_expired"
}
```

Called by voice-service on receipt of WS `auth`. Voice-service caches
session details locally so subsequent UDP packets don't hit L2J.

---

## 7. Open decisions (need user confirmation)

1. **Token delivery to client**: option A (Say2 system message that DLL parses) vs option B (custom L2J packet that DLL intercepts via hook). A is simpler v1, B is cleaner production.
2. **Should we use protobuf instead of JSON for WS?** JSON easier to debug, protobuf saves ~30% bandwidth. WS is low-volume so probably not worth the complexity.
3. **NAT/STUN/TURN**: assume client → service UDP works directly (most home NATs allow outbound UDP + keepalive). If symmetric NAT breaks, add STUN/TURN later.
4. **Server-side mixing vs client-side mixing**: prompt says client-side (proximity routes packets unmixed, client renders 3D). Confirm: party/clan/ally also unmixed?
5. **Encryption of UDP audio**: plaintext (current proposal) vs lightweight (ChaCha20 stream with session-derived key). Plaintext saves CPU and is fine if session_id is hard to guess.
