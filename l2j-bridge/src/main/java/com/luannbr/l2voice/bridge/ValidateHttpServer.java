package com.luannbr.l2voice.bridge;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.logging.Level;
import java.util.logging.Logger;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

/**
 * Embedded HTTP server that answers {@code POST /voice/validate}.
 * The voice-service calls this once per WS auth.
 *
 * <p>Request body (JSON):
 * <pre>{ "token": "...", "player_id": 1234567 }</pre>
 *
 * <p>Response 200:
 * <pre>{ "valid": true, "session_id": 305419896, "player": { ... } }</pre>
 *
 * <p>Response 401:
 * <pre>{ "valid": false, "reason": "token_expired" }</pre>
 *
 * <p>Uses {@code com.sun.net.httpserver} (built-in JDK) to keep the
 * dependency footprint at just Jedis.
 */
final class ValidateHttpServer {

    private static final Logger log = Logger.getLogger(ValidateHttpServer.class.getName());

    private final String bind;
    private final int port;
    private final TokenIssuer tokens;
    private L2WorldRef worldRef;
    private HttpServer server;

    ValidateHttpServer(String bind, int port, TokenIssuer tokens) {
        this.bind = bind;
        this.port = port;
        this.tokens = tokens;
    }

    void start() {
        try {
            worldRef = new L2WorldRef();
        } catch (Throwable t) {
            // Running outside the GS JVM (e.g. smoke test). /voice/check
            // will respond with online=false rather than 500.
            log.warning("L2WorldRef bind failed; /voice/check will report offline only: " + t);
        }
        try {
            server = HttpServer.create(new InetSocketAddress(bind, port), 0);
            server.createContext("/voice/validate", this::handleValidate);
            server.createContext("/voice/check",    this::handleCheck);
            server.createContext("/voice/whoami",   this::handleWhoami);
            server.createContext("/voice/name",     this::handleName);
            server.setExecutor(null);     // single thread; load is trivial
            server.start();
            log.info("validate HTTP server listening on " + bind + ":" + port);
        } catch (IOException e) {
            log.log(Level.SEVERE, "failed to start HTTP server", e);
        }
    }

    void stop() {
        if (server != null) server.stop(1);
    }

    private void handleValidate(HttpExchange ex) throws IOException {
        if (!"POST".equalsIgnoreCase(ex.getRequestMethod())) {
            respond(ex, 405, "{\"valid\":false,\"reason\":\"method_not_allowed\"}");
            return;
        }
        byte[] body;
        try (InputStream is = ex.getRequestBody()) {
            body = is.readAllBytes();
        }
        String s = new String(body, StandardCharsets.UTF_8);
        String token = extractString(s, "token");
        int playerId = extractInt(s, "player_id");
        if (token == null || playerId == 0) {
            respond(ex, 400, "{\"valid\":false,\"reason\":\"malformed_request\"}");
            return;
        }
        if (!tokens.validate(playerId, token)) {
            respond(ex, 401, "{\"valid\":false,\"reason\":\"token_invalid\"}");
            return;
        }
        long sessionId = ((long) playerId) & 0xFFFFFFFFL;  // simple 1:1 mapping for MVP
        String body200 = "{\"valid\":true,\"session_id\":" + sessionId +
                ",\"player\":{\"player_id\":" + playerId + "}}";
        respond(ex, 200, body200);
    }

    /**
     * GET /voice/check?player_id=N
     *
     * Lightweight liveness check used by the voice-service stub-auth:
     * "is this player_id currently online in L2World?" Returns 200 with
     *   {"online":true|false,"player_id":N}
     * regardless of outcome — voice-service decides what to do with the
     * answer. No HMAC, no nonce; the binding here is "if the player is
     * actually logged in on the GS, accept their voice session".
     */
    private void handleCheck(HttpExchange ex) throws IOException {
        if (!"GET".equalsIgnoreCase(ex.getRequestMethod())) {
            respond(ex, 405, "{\"online\":false,\"reason\":\"method_not_allowed\"}");
            return;
        }
        String q = ex.getRequestURI().getRawQuery();
        int pid = parseQueryInt(q, "player_id");
        if (pid == 0) {
            respond(ex, 400, "{\"online\":false,\"reason\":\"missing_player_id\"}");
            return;
        }
        boolean online = false;
        if (worldRef != null) {
            try { online = worldRef.containsPlayer(pid); }
            catch (Throwable t) {
                log.log(Level.WARNING, "containsPlayer failed", t);
            }
        }
        respond(ex, 200, "{\"online\":" + online + ",\"player_id\":" + pid + "}");
    }

    /**
     * GET /voice/whoami?ip=X&ports=A,B,C
     *
     * Identity resolver. The voice-service forwards (client_ip,
     * candidate ports) — bridge iterates L2World.getAllPlayers and
     * matches against each player's L2GameClient remote socket. Returns
     *   {"player_id": N}  if a match, 0 if none.
     */
    private void handleWhoami(HttpExchange ex) throws IOException {
        if (!"GET".equalsIgnoreCase(ex.getRequestMethod())) {
            respond(ex, 405, "{\"player_id\":0,\"reason\":\"method_not_allowed\"}");
            return;
        }
        String q = ex.getRequestURI().getRawQuery();
        String ip = parseQueryStr(q, "ip");
        String portsCsv = parseQueryStr(q, "ports");
        if (ip == null || portsCsv == null) {
            respond(ex, 400, "{\"player_id\":0,\"reason\":\"missing_ip_or_ports\"}");
            return;
        }
        java.util.Set<Integer> ports = new java.util.HashSet<>();
        for (String s : portsCsv.split(",")) {
            try { ports.add(Integer.parseInt(s.trim())); }
            catch (NumberFormatException nfe) { /* skip */ }
        }
        int pid = 0;
        if (worldRef != null && !ports.isEmpty()) {
            try { pid = worldRef.findPlayerByConnection(ip, ports); }
            catch (Throwable t) {
                log.log(Level.WARNING, "findPlayerByConnection failed", t);
            }
        }
        log.info("whoami ip=" + ip + " ports=" + ports + " -> player_id=" + pid);
        respond(ex, 200, "{\"player_id\":" + pid + "}");
    }

    /** GET /voice/name?player_id=N → {"player_id":N,"name":"..."} */
    private void handleName(HttpExchange ex) throws IOException {
        if (!"GET".equalsIgnoreCase(ex.getRequestMethod())) {
            respond(ex, 405, "{\"name\":null}");
            return;
        }
        String q = ex.getRequestURI().getRawQuery();
        int pid = parseQueryInt(q, "player_id");
        if (pid == 0) {
            respond(ex, 400, "{\"name\":null,\"reason\":\"missing_player_id\"}");
            return;
        }
        String name = null;
        if (worldRef != null) {
            try { name = worldRef.getPlayerName(pid); }
            catch (Throwable t) {
                log.log(Level.WARNING, "getPlayerName failed", t);
            }
        }
        String quoted = name == null ? "null"
            : "\"" + name.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
        respond(ex, 200, "{\"player_id\":" + pid + ",\"name\":" + quoted + "}");
    }

    private static String parseQueryStr(String q, String key) {
        if (q == null) return null;
        for (String pair : q.split("&")) {
            int eq = pair.indexOf('=');
            if (eq < 0) continue;
            if (pair.substring(0, eq).equals(key)) {
                try {
                    return java.net.URLDecoder.decode(pair.substring(eq + 1),
                            java.nio.charset.StandardCharsets.UTF_8);
                } catch (Exception e) { return null; }
            }
        }
        return null;
    }

    private static int parseQueryInt(String q, String key) {
        if (q == null) return 0;
        for (String pair : q.split("&")) {
            int eq = pair.indexOf('=');
            if (eq < 0) continue;
            String k = pair.substring(0, eq);
            String v = pair.substring(eq + 1);
            if (k.equals(key)) {
                try { return Integer.parseInt(v); }
                catch (NumberFormatException nfe) { return 0; }
            }
        }
        return 0;
    }

    private static void respond(HttpExchange ex, int code, String body) throws IOException {
        byte[] b = body.getBytes(StandardCharsets.UTF_8);
        ex.getResponseHeaders().add("Content-Type", "application/json; charset=utf-8");
        ex.sendResponseHeaders(code, b.length);
        try (OutputStream os = ex.getResponseBody()) { os.write(b); }
    }

    /** Tiny ad-hoc JSON extractors (only used for the validate request). */
    private static String extractString(String json, String key) {
        String n = "\"" + key + "\":\"";
        int i = json.indexOf(n);
        if (i < 0) return null;
        int s = i + n.length();
        int e = json.indexOf('"', s);
        return e < 0 ? null : json.substring(s, e);
    }

    private static int extractInt(String json, String key) {
        String n = "\"" + key + "\":";
        int i = json.indexOf(n);
        if (i < 0) return 0;
        int s = i + n.length();
        int e = s;
        while (e < json.length() && (json.charAt(e) == '-' ||
                Character.isDigit(json.charAt(e)))) e++;
        try { return Integer.parseInt(json.substring(s, e)); }
        catch (NumberFormatException nfe) { return 0; }
    }
}
