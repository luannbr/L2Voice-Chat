package com.luannbr.l2voice.bridge;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Issues and validates short-lived HMAC tokens for voice auth.
 *
 * <p>Token format (URL-safe base64): {@code HMAC-SHA256(secret,
 * playerId || ":" || issuedAtMs || ":" || nonceHex)}, prefixed with
 * {@code playerId.issuedAtMs.nonceHex.} The voice-service POSTs the
 * token + player_id to {@link ValidateHttpServer}, we recompute and
 * compare in constant time.
 *
 * <p>Tokens expire after {@link #TTL_MS}. After validation the
 * (playerId, nonce) pair is consumed (one-shot) to make replay
 * harmless even if the token leaks.
 */
final class TokenIssuer {

    private static final long TTL_MS = 60_000;  // 1 minute window

    private final byte[] secret;
    private final java.security.SecureRandom rng = new java.security.SecureRandom();
    /** Consumed (playerId, nonce) → consumed_at_ms for replay defense. */
    private final ConcurrentMap<String, Long> consumed = new ConcurrentHashMap<>();

    TokenIssuer(String secretAscii) {
        this.secret = secretAscii.getBytes(StandardCharsets.UTF_8);
    }

    String issue(int playerId) {
        long now = System.currentTimeMillis();
        byte[] nonce = new byte[12];
        rng.nextBytes(nonce);
        String nonceHex = toHex(nonce);
        String payload  = playerId + "." + now + "." + nonceHex;
        String sig      = b64url(hmac(payload.getBytes(StandardCharsets.UTF_8)));
        return payload + "." + sig;
    }

    /** Returns true iff token is valid for the given playerId and not yet consumed. */
    boolean validate(int playerId, String token) {
        if (token == null) return false;
        String[] parts = token.split("\\.");
        if (parts.length != 4) return false;
        int    tokPlayer;
        long   issued;
        try {
            tokPlayer = Integer.parseInt(parts[0]);
            issued    = Long.parseLong(parts[1]);
        } catch (NumberFormatException nfe) { return false; }
        if (tokPlayer != playerId) return false;
        long age = System.currentTimeMillis() - issued;
        if (age < 0 || age > TTL_MS) return false;

        String payload = parts[0] + "." + parts[1] + "." + parts[2];
        byte[] expected = hmac(payload.getBytes(StandardCharsets.UTF_8));
        byte[] given;
        try { given = Base64.getUrlDecoder().decode(parts[3]); }
        catch (IllegalArgumentException e) { return false; }
        if (!MessageDigest.isEqual(expected, given)) return false;

        String key = playerId + ":" + parts[2];
        if (consumed.putIfAbsent(key, System.currentTimeMillis()) != null) {
            return false;        // replay attempt
        }
        pruneExpired();
        return true;
    }

    private byte[] hmac(byte[] data) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret, "HmacSHA256"));
            return mac.doFinal(data);
        } catch (Exception e) {
            throw new IllegalStateException("HmacSHA256 unavailable", e);
        }
    }

    private void pruneExpired() {
        long cutoff = System.currentTimeMillis() - TTL_MS;
        consumed.values().removeIf(t -> t < cutoff);
    }

    private static String b64url(byte[] b) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(b);
    }

    private static String toHex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte v : b) sb.append(String.format("%02x", v));
        return sb.toString();
    }
}
