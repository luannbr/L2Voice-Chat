package com.luannbr.l2voice.bridge;

import java.util.Properties;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Logger;

/**
 * Entry point for the l2voice bridge inside the L2J GameServer.
 *
 * <p>Call {@link #start(Properties)} once from the GS startup
 * sequence (see {@code l2j-bridge/README.md} for the recommended
 * patch site). Call {@link #stop()} on shutdown if you wire it in.
 *
 * <p>This class only orchestrates — the actual work lives in:
 * <ul>
 *   <li>{@link RedisPublisher} — pub/sub JSON events to the voice service
 *   <li>{@link PositionPoller} — periodic position broadcast from L2World
 *   <li>{@link TokenIssuer}    — HMAC voice token at login
 *   <li>{@link ValidateHttpServer} — {@code POST /voice/validate}
 * </ul>
 */
public final class VoiceBridge {

    private static final Logger log = Logger.getLogger(VoiceBridge.class.getName());
    private static final AtomicBoolean started = new AtomicBoolean(false);

    private static RedisPublisher        publisher;
    private static PositionPoller        poller;
    private static TokenIssuer           tokens;
    private static ValidateHttpServer    http;
    // Multi-VPS: one link per configured voice-server URL. Phase C
    // populates this from `l2voice.voice_server.urls` (csv) — Phase B
    // had a single `l2voice.voice_server.url`, kept as fallback.
    private static final java.util.List<VoiceServerLink> links =
            new java.util.ArrayList<>();

    private VoiceBridge() {}

    /**
     * Starts all subsystems. Properties (with defaults):
     * <pre>
     *   l2voice.redis.host       = 127.0.0.1
     *   l2voice.redis.port       = 6379
     *   l2voice.redis.channel    = l2voice:events
     *   l2voice.http.bind        = 0.0.0.0
     *   l2voice.http.port        = 17668
     *   l2voice.hmac.secret      = (REQUIRED — pick a long random string)
     *   l2voice.position.hz      = 5
     *   l2voice.position.minDelta= 50
     * </pre>
     */
    public static synchronized void start(Properties cfg) {
        if (!started.compareAndSet(false, true)) {
            log.warning("VoiceBridge.start called twice — ignoring");
            return;
        }
        String secret = cfg.getProperty("l2voice.hmac.secret", "");
        if (secret.isEmpty()) {
            log.severe("l2voice.hmac.secret is empty — refusing to start");
            started.set(false);
            return;
        }

        publisher = new RedisPublisher(
                cfg.getProperty("l2voice.redis.host", "127.0.0.1"),
                Integer.parseInt(cfg.getProperty("l2voice.redis.port", "6379")),
                cfg.getProperty("l2voice.redis.channel", "l2voice:events"));
        publisher.start();

        tokens = new TokenIssuer(secret);

        int hz       = Integer.parseInt(cfg.getProperty("l2voice.position.hz", "5"));
        int minDelta = Integer.parseInt(cfg.getProperty("l2voice.position.minDelta", "50"));
        poller = new PositionPoller(publisher, hz, minDelta);
        poller.start();

        http = new ValidateHttpServer(
                cfg.getProperty("l2voice.http.bind", "0.0.0.0"),
                Integer.parseInt(cfg.getProperty("l2voice.http.port", "17668")),
                tokens);
        http.start();

        // Outbound links to one or more voice-servers (Phase C: multi-VPS).
        // Properties accepted, in priority order:
        //   l2voice.voice_server.urls = ws://br:17667/bridge,ws://us:17667/bridge,...
        //   l2voice.voice_server.url  = ws://<vps>:17667/bridge  (legacy single)
        // Empty value disables the link path; the bridge keeps publishing
        // events to Redis (if configured) and serving HTTP callbacks for
        // backwards-compat.
        String urlList = cfg.getProperty("l2voice.voice_server.urls", "").trim();
        if (urlList.isEmpty()) {
            urlList = cfg.getProperty("l2voice.voice_server.url", "").trim();
        }
        if (!urlList.isEmpty()) {
            String gsName  = cfg.getProperty("l2voice.gs.name", "L2J");
            String version = "0.1.0";
            for (String raw : urlList.split(",")) {
                String url = raw.trim();
                if (url.isEmpty()) continue;
                VoiceServerLink l = new VoiceServerLink(url, gsName, version);
                l.start();
                links.add(l);
                log.info("l2voice voice-server link enabled: " + url);
            }
            log.info("l2voice: " + links.size() + " voice-server link(s) started");
        }

        log.info("l2voice bridge started");
    }

    /** Stops all subsystems. Safe to call from GS shutdown hook. */
    public static synchronized void stop() {
        if (!started.compareAndSet(true, false)) return;
        for (VoiceServerLink l : links) {
            try { l.stop(); } catch (Throwable t) { /* swallow */ }
        }
        links.clear();
        if (poller    != null) poller.stop();
        if (http      != null) http.stop();
        if (publisher != null) publisher.stop();
        log.info("l2voice bridge stopped");
    }

    /**
     * Called from a GS hook point (e.g., EnterWorld packet handler) to
     * mint a voice token and ship it to the client. See README.md.
     *
     * @return the base64url-encoded HMAC token; null if bridge isn't running.
     */
    public static String issueTokenOnLogin(int playerObjectId) {
        if (!started.get() || tokens == null) return null;
        publisher.publishPlayerLogin(playerObjectId);
        return tokens.issue(playerObjectId);
    }

    /** Notify the voice-service that this player has left. */
    public static void notifyLogout(int playerObjectId) {
        if (!started.get() || publisher == null) return;
        publisher.publishPlayerLogout(playerObjectId);
    }
}
