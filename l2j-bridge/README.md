# l2j-bridge (Phase 4)

Java module that lives **inside** the l2emuproject Essence 542
GameServer JVM. Two jobs:

1. **Push player state to the voice-service** over Redis pub/sub.
   Today: position events (5 Hz throttled) + player login/logout.
   Later: party/clan/ally/instance events.
2. **Issue and validate voice tokens.** Each player gets an HMAC
   token at login; the voice-service redeems it via `POST /voice/validate`.

The bridge does NOT modify the GS code at compile time. You drop the
shaded jar into `gameserver/lib/` and add a single line to call
`VoiceBridge.start(...)` from the GS startup sequence. Reflection is
used to read player positions, so the bridge survives minor GS
refactors.

## Build

```bat
mvn -f l2j-bridge\pom.xml package
```

Output: `l2j-bridge\target\l2voice-bridge-0.1.0.jar`.

If the GS classes haven't been built yet, run the GS Ant build first
or adjust `<l2j.gs.jar>` in `pom.xml` to point at an existing build
output. (Build path is the Ant default
`build/gameserver/l2emu-gs.jar`.)

## Deploy

1. Build the GS as usual (`build.bat` in `Source-GS_SR-542-main`).
2. Copy `l2voice-bridge-0.1.0.jar` and `jedis-5.1.2.jar` into the
   GS `gameserver/lib/` directory.
3. Create `gameserver/config/l2voice.properties`:

```properties
l2voice.redis.host       = 127.0.0.1
l2voice.redis.port       = 6379
l2voice.redis.channel    = l2voice:events
l2voice.http.bind        = 0.0.0.0
l2voice.http.port        = 17668
l2voice.hmac.secret      = REPLACE-WITH-A-LONG-RANDOM-STRING
l2voice.position.hz      = 5
l2voice.position.minDelta= 50
```

4. Patch the GS startup. Open `net.l2emuproject.gameserver.GameServer`
   (the class with the `main(String[])`) and after the world has
   finished loading, add:

```java
java.util.Properties vp = new java.util.Properties();
try (var in = new java.io.FileInputStream("config/l2voice.properties")) {
    vp.load(in);
}
com.luannbr.l2voice.bridge.VoiceBridge.start(vp);
```

5. Patch `EnterWorld` (the packet handler called at character entry
   into the world) to mint a voice token and Say2-broadcast it to the
   client. Add **after** the existing `sendPacket(...)` calls:

```java
String voiceToken =
    com.luannbr.l2voice.bridge.VoiceBridge.issueTokenOnLogin(
        activeChar.getObjectId());
if (voiceToken != null) {
    activeChar.sendMessage("\\u0001VOICE:" + voiceToken);
}
```

The DLL (`l2voice.dll`) intercepts any chat line beginning with the
`VOICE:` sentinel and extracts the token. (We use a private
control-char prefix so the message is invisible to other chat
listeners.)

6. Patch the logout flow (e.g., `L2PcInstance.deleteMe` or the
   disconnect packet handler):

```java
com.luannbr.l2voice.bridge.VoiceBridge.notifyLogout(getObjectId());
```

## Verifying

After GS restart:

- Connect with a character → GS log should show
  `l2voice bridge started` and `validate HTTP server listening on
  0.0.0.0:17668`.
- The character's first chat line should be the encoded
  `VOICE:...` (visible in raw packet dumps; invisible in-client).
- `redis-cli SUBSCRIBE l2voice:events` should print `player_login`
  followed by `position` updates as the character moves.

## Class layout

```
l2j-bridge/
├── pom.xml
├── README.md   (this file)
└── src/main/java/com/luannbr/l2voice/bridge/
    ├── VoiceBridge.java          public entry point
    ├── RedisPublisher.java       background thread + JSON envelopes
    ├── PositionPoller.java       reflective L2World scan, 5 Hz default
    ├── TokenIssuer.java          HMAC-SHA256, 60 s TTL, one-shot replay defense
    └── ValidateHttpServer.java   POST /voice/validate
```

## Tradeoffs / scope

- **Reflection** instead of hard compile-time linkage. Costs a little
  RAM (cached `Method`s) and a slight per-call overhead. Buys: the
  bridge compiles even if the GS classes aren't on the classpath, and
  refactors that rename fields don't break us silently — they throw
  `NoSuchMethodException` at startup.
- **Position polling** instead of event hooks. The L2J GS doesn't
  expose a clean position-changed listener; movement is implicit in
  AI ticks. 5 Hz polling is far cheaper than patching every code path
  that calls `setXYZ`.
- **No Olympiad / siege / multibox handling yet** — that's Phase 5.
