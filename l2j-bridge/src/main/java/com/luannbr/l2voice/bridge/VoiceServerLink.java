package com.luannbr.l2voice.bridge;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.WebSocket;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Outbound persistent WebSocket to one voice-server (POC, Phase 5+
 * multi-VPS prep).
 *
 * <p>Replaces the inbound HTTP-callback architecture (where the
 * voice-server calls back into the bridge at {@code /voice/whoami}
 * etc.) with a single outbound connection. The bridge initiates the
 * connection so it traverses NAT cleanly; the voice-server is just a
 * passive listener. Multiple {@code VoiceServerLink} instances can be
 * started from {@link VoiceBridge} to fan out to multiple VPSes for
 * regional latency.
 *
 * <p>POC scope (this file): connect, exchange a hello, run a periodic
 * ping-pong with RTT measurement. Auto-reconnect with exponential
 * backoff. No replacement of existing flows yet — the bridge keeps
 * Redis + HTTP callbacks running in parallel.
 *
 * <p>Wire format mirrors the server side (control/bridge_link.go):
 * <pre>
 *   bridge → server  {"type":"hello", "gs":"&lt;name&gt;", "v":"&lt;version&gt;"}
 *   server → bridge  {"type":"welcome"}
 *   either           {"type":"ping","t":&lt;epoch_ms&gt;}
 *   either           {"type":"pong","t":&lt;echoed_t&gt;}
 * </pre>
 */
final class VoiceServerLink {

    private static final Logger log = Logger.getLogger(VoiceServerLink.class.getName());
    private static final Duration PING_INTERVAL = Duration.ofSeconds(5);
    private static final long BACKOFF_INITIAL_MS = 1_000L;
    private static final long BACKOFF_MAX_MS = 30_000L;

    // Phase B: every active link registers itself here so RedisPublisher
    // can fan event messages out over WS in addition to (or instead of)
    // Redis. Read-mostly — adds happen on connect, removes on close.
    private static final List<VoiceServerLink> activeLinks = new CopyOnWriteArrayList<>();

    /** Broadcasts a JSON event envelope to every connected
     *  voice-server link. Called by RedisPublisher every time it
     *  enqueues an event. No-op when no links are connected. */
    static void broadcastEvent(String envelopeJson) {
        if (activeLinks.isEmpty()) return;
        // Wrap the Redis-style envelope as a typed bridge frame so the
        // voice-server dispatcher can distinguish events from RPC.
        String wrapped = "{\"type\":\"event\",\"payload\":" + envelopeJson + "}";
        for (VoiceServerLink l : activeLinks) {
            l.sendIfOpen(wrapped);
        }
    }

    /** Best-effort, non-blocking send over the currently-open socket.
     *  Failures are swallowed — the reconnect loop will recover. */
    private void sendIfOpen(String text) {
        WebSocket sock = ws.get();
        if (sock == null) return;
        try { sock.sendText(text, true); }
        catch (Exception ignored) {}
    }

    private final URI url;
    private final String gsName;
    private final String bridgeVersion;

    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private final AtomicReference<WebSocket> ws = new AtomicReference<>();
    private Thread worker;

    VoiceServerLink(String url, String gsName, String bridgeVersion) {
        this.url = URI.create(url);
        this.gsName = gsName;
        this.bridgeVersion = bridgeVersion;
    }

    void start() {
        worker = new Thread(this::run, "l2voice-server-link");
        worker.setDaemon(true);
        worker.start();
    }

    void stop() {
        stopping.set(true);
        WebSocket cur = ws.get();
        if (cur != null) {
            cur.sendClose(WebSocket.NORMAL_CLOSURE, "bye").join();
        }
        if (worker != null) worker.interrupt();
    }

    /** Reconnect loop with exponential backoff. */
    private void run() {
        long backoff = BACKOFF_INITIAL_MS;
        while (!stopping.get()) {
            try {
                connectAndPump();
                // Connection ended cleanly — try again immediately.
                backoff = BACKOFF_INITIAL_MS;
            } catch (Exception e) {
                log.log(Level.WARNING,
                        "voice-link to " + url + " failed: " + e.getMessage()
                                + " (retry in " + (backoff / 1000) + "s)");
                try { Thread.sleep(backoff); }
                catch (InterruptedException ie) { return; }
                if (backoff < BACKOFF_MAX_MS) backoff *= 2;
            }
        }
    }

    /** Opens the socket, runs until it closes. */
    private void connectAndPump() throws Exception {
        HttpClient client = HttpClient.newHttpClient();
        // Bind the L2World reflection helper once per connection so
        // RPC handlers can call into it from the listener thread.
        L2WorldRef world;
        try { world = new L2WorldRef(); }
        catch (Exception e) {
            log.log(Level.WARNING, "voice-link: cannot bind L2WorldRef — RPC handlers disabled", e);
            world = null;
        }
        Listener listener = new Listener(world);
        WebSocket socket = client
                .newWebSocketBuilder()
                .connectTimeout(Duration.ofSeconds(10))
                .buildAsync(url, listener)
                .get();
        listener.setSocket(socket);
        ws.set(socket);
        activeLinks.add(this);
        log.info("voice-link connected to " + url);

        // Hello.
        String hello = "{\"type\":\"hello\",\"gs\":\"" + escape(gsName)
                + "\",\"v\":\"" + escape(bridgeVersion) + "\"}";
        socket.sendText(hello, true).get();

        // Periodic ping until the listener signals the socket closed.
        while (!stopping.get() && !listener.closed.get()) {
            try { Thread.sleep(PING_INTERVAL.toMillis()); }
            catch (InterruptedException ie) { break; }
            long now = System.currentTimeMillis();
            String ping = "{\"type\":\"ping\",\"t\":" + now + "}";
            try {
                socket.sendText(ping, true).get();
            } catch (Exception e) {
                throw new RuntimeException("ping send failed", e);
            }
        }
        ws.set(null);
        activeLinks.remove(this);
        if (!listener.closed.get()) {
            socket.sendClose(WebSocket.NORMAL_CLOSURE, "shutdown");
        }
    }

    private static String escape(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /** Receives frames from the socket. Logs pongs (with RTT), handles
     *  RPC requests (whoami / name) via L2WorldRef, flips `closed`
     *  when the peer closes. */
    private static final class Listener implements WebSocket.Listener {
        final AtomicBoolean closed = new AtomicBoolean(false);
        private final StringBuilder buf = new StringBuilder();
        private final L2WorldRef world;
        private volatile WebSocket socket;

        Listener(L2WorldRef world) { this.world = world; }

        void setSocket(WebSocket socket) { this.socket = socket; }

        @Override
        public void onOpen(WebSocket ws) {
            WebSocket.Listener.super.onOpen(ws);
        }

        @Override
        public CompletionStage<?> onText(WebSocket ws, CharSequence data, boolean last) {
            buf.append(data);
            if (!last) {
                ws.request(1);
                return null;
            }
            String msg = buf.toString();
            buf.setLength(0);
            handleMessage(msg);
            ws.request(1);
            return null;
        }

        @Override
        public CompletionStage<?> onClose(WebSocket ws, int statusCode, String reason) {
            closed.set(true);
            log.info("voice-link closed (status=" + statusCode + " reason=\"" + reason + "\")");
            return null;
        }

        @Override
        public void onError(WebSocket ws, Throwable error) {
            closed.set(true);
            log.log(Level.WARNING, "voice-link error", error);
        }

        private void handleMessage(String msg) {
            String type = jsonString(msg, "type");
            if (type == null) return;
            switch (type) {
                case "welcome":
                    log.info("voice-link: server says welcome");
                    break;
                case "pong":
                    long t = jsonLong(msg, "t");
                    log.info("voice-link: rtt=" + (System.currentTimeMillis() - t) + "ms");
                    break;
                case "ping":
                    long pt = jsonLong(msg, "t");
                    send("{\"type\":\"pong\",\"t\":" + pt + "}");
                    break;
                case "rpc_whoami":
                    handleWhoami(msg);
                    break;
                case "rpc_name":
                    handleName(msg);
                    break;
                default:
                    log.fine("voice-link msg: " + msg);
            }
        }

        /** Phase A: server-side called HTTP /voice/whoami; now does
         *  it over this WS link. Body: {id, ip, ports:[int,...]}. */
        private void handleWhoami(String msg) {
            long id = jsonLong(msg, "id");
            String ip = jsonString(msg, "ip");
            int[] ports = jsonIntArray(msg, "ports");
            int playerId = 0;
            if (world != null && ip != null && ports.length > 0) {
                try {
                    java.util.Set<Integer> set = new java.util.HashSet<>();
                    for (int p : ports) set.add(p);
                    playerId = world.findPlayerByConnection(ip, set);
                } catch (Exception e) {
                    log.log(Level.WARNING, "rpc_whoami failed", e);
                }
            }
            log.info("rpc_whoami: ip=" + ip + " ports=" + java.util.Arrays.toString(ports)
                    + " -> player_id=" + playerId);
            send("{\"type\":\"rpc_whoami_resp\",\"id\":" + id
                    + ",\"player_id\":" + playerId + "}");
        }

        /** Body: {id, player_id}. Replies with the character name or
         *  empty string if not online. */
        private void handleName(String msg) {
            long id = jsonLong(msg, "id");
            long pid = jsonLong(msg, "player_id");
            String name = "";
            if (world != null && pid > 0) {
                try { name = world.getPlayerName((int) pid); }
                catch (Exception e) { log.log(Level.WARNING, "rpc_name failed", e); }
                if (name == null) name = "";
            }
            log.info("rpc_name: player_id=" + pid + " -> \"" + name + "\"");
            send("{\"type\":\"rpc_name_resp\",\"id\":" + id
                    + ",\"name\":\"" + escapeJson(name) + "\"}");
        }

        private void send(String s) {
            WebSocket sock = this.socket;
            if (sock == null) return;
            try { sock.sendText(s, true); }
            catch (Exception e) { log.log(Level.WARNING, "voice-link send failed", e); }
        }
    }

    private static String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /** Parses `"key":[1,2,3]` into an int[]. Stops at the matching ]. */
    private static int[] jsonIntArray(String s, String key) {
        String needle = "\"" + key + "\":[";
        int i = s.indexOf(needle);
        if (i < 0) return new int[0];
        i += needle.length();
        int end = s.indexOf(']', i);
        if (end < 0) return new int[0];
        String body = s.substring(i, end).trim();
        if (body.isEmpty()) return new int[0];
        String[] parts = body.split(",");
        int[] out = new int[parts.length];
        for (int k = 0; k < parts.length; ++k) {
            try { out[k] = Integer.parseInt(parts[k].trim()); }
            catch (Exception e) { out[k] = 0; }
        }
        return out;
    }

    // Tiny ad-hoc JSON extractors — enough for the POC's flat objects.
    private static String jsonString(String s, String key) {
        String needle = "\"" + key + "\":\"";
        int i = s.indexOf(needle);
        if (i < 0) return null;
        i += needle.length();
        int j = s.indexOf('"', i);
        return (j < 0) ? null : s.substring(i, j);
    }
    private static long jsonLong(String s, String key) {
        String needle = "\"" + key + "\":";
        int i = s.indexOf(needle);
        if (i < 0) return 0;
        i += needle.length();
        int j = i;
        while (j < s.length() && (Character.isDigit(s.charAt(j)) || s.charAt(j) == '-')) ++j;
        try { return Long.parseLong(s.substring(i, j)); }
        catch (Exception e) { return 0; }
    }
}
