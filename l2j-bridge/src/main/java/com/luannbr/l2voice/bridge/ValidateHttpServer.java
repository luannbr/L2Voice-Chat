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
    private HttpServer server;

    ValidateHttpServer(String bind, int port, TokenIssuer tokens) {
        this.bind = bind;
        this.port = port;
        this.tokens = tokens;
    }

    void start() {
        try {
            server = HttpServer.create(new InetSocketAddress(bind, port), 0);
            server.createContext("/voice/validate", this::handle);
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

    private void handle(HttpExchange ex) throws IOException {
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
