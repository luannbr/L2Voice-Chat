package com.luannbr.l2voice.bridge;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Periodically iterates online players and publishes voice-relevant
 * state changes to Redis:
 *   - position (throttled by minDelta cm + heartbeat every 3 s)
 *   - clan_change   (when clanId / isClanLeader changes)
 *   - ally_change   (when allyId changes)
 *   - party_change  (when partyId changes)
 *   - clan_leader_change (when a player became their clan's leader)
 *
 * <p>State diffing is done against an in-memory {@code Map<Integer,
 * PlayerSnapshot>}. First-sight of a player publishes ALL fields
 * (we treat the previous snapshot as zero), so the voice-service
 * picks up the initial clan/ally/party state without waiting for a
 * change.
 *
 * <p>L2J class names are accessed via reflection in {@link
 * L2WorldRef}; this poller stays decoupled from compile-time L2J
 * types.
 */
final class PositionPoller {

    private static final Logger log = Logger.getLogger(PositionPoller.class.getName());

    private final RedisPublisher pub;
    private final int hz;
    private final int minDelta;

    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private Thread thread;

    /** Per-player last snapshot for delta detection. */
    private final Map<Integer, L2WorldRef.PlayerSnapshot> last = new HashMap<>();

    /** Last position-publish wall-clock-ms per player. Heartbeat
     *  re-publishes position even when stationary so the voice-service
     *  picks it up even if it allocated the session after first-sight. */
    private final Map<Integer, Long> lastPosPublishMs = new HashMap<>();
    private static final long HEARTBEAT_MS = 3000;

    PositionPoller(RedisPublisher pub, int hz, int minDelta) {
        this.pub      = pub;
        this.hz       = Math.max(1, hz);
        this.minDelta = Math.max(1, minDelta);
    }

    void start() {
        thread = new Thread(this::run, "l2voice-pos-poller");
        thread.setDaemon(true);
        thread.start();
    }

    void stop() {
        stopping.set(true);
        if (thread != null) thread.interrupt();
    }

    private void run() {
        long periodMs = 1000L / hz;
        L2WorldRef world;
        try {
            world = new L2WorldRef();
        } catch (Exception e) {
            log.log(Level.SEVERE, "could not bind L2World via reflection; poller disabled", e);
            return;
        }
        log.info("PositionPoller started at " + hz + " Hz (delta " + minDelta + "cm)");
        long lastStats = 0;
        int publishedSinceStats = 0;
        while (!stopping.get()) {
            long t0 = System.currentTimeMillis();
            try {
                final int[] cnt = {0, 0};   // seen, events_published
                world.forEachSnapshot(s -> {
                    cnt[0]++;
                    publishedThisTick = 0;
                    tickPlayer(s);
                    cnt[1] += publishedThisTick;
                });
                publishedSinceStats += cnt[1];
                if (t0 - lastStats > 30_000) {
                    log.info("PositionPoller: " + cnt[0] + " players visible, "
                            + publishedSinceStats + " events in last "
                            + ((t0 - lastStats) / 1000) + "s");
                    lastStats = t0;
                    publishedSinceStats = 0;
                }
            } catch (Exception e) {
                log.log(Level.WARNING, "poll iteration failed", e);
            }
            long sleep = periodMs - (System.currentTimeMillis() - t0);
            if (sleep > 0) {
                try { Thread.sleep(sleep); }
                catch (InterruptedException ie) { return; }
            }
        }
    }

    private int publishedThisTick = 0;

    private void tickPlayer(L2WorldRef.PlayerSnapshot s) {
        L2WorldRef.PlayerSnapshot prev = last.get(s.objectId);
        long now = System.currentTimeMillis();

        // ---- clan / ally / party / leader diffs ----
        // First-sight: publish current values. Subsequent ticks only
        // publish on change.
        if (prev == null || prev.clanId != s.clanId || prev.isClanLeader != s.isClanLeader) {
            pub.publishClanChange(s.objectId, s.clanId, s.isClanLeader);
            publishedThisTick++;
            // If THIS player just became their clan's leader, also
            // emit a clan_leader_change event so the voice-service
            // updates its Clan.LeaderID atomically.
            if (s.isClanLeader && s.clanId != 0
                    && (prev == null || !prev.isClanLeader)) {
                pub.publishClanLeaderChange(s.clanId, s.objectId);
                publishedThisTick++;
            }
        }
        if (prev == null || prev.allyId != s.allyId) {
            pub.publishAllyChange(s.objectId, s.allyId);
            publishedThisTick++;
        }
        if (prev == null || prev.partyId != s.partyId) {
            pub.publishPartyChange(s.objectId, s.partyId);
            publishedThisTick++;
        }
        if (prev == null || prev.instanceId != s.instanceId) {
            pub.publishInstanceChange(s.objectId, s.instanceId);
            publishedThisTick++;
        }

        // ---- position (movement-throttled + heartbeat) ----
        Long lastMs = lastPosPublishMs.get(s.objectId);
        boolean heartbeatDue = lastMs == null || (now - lastMs) >= HEARTBEAT_MS;
        boolean movedEnough = prev == null
                || Math.abs(prev.x - s.x) >= minDelta
                || Math.abs(prev.y - s.y) >= minDelta
                || Math.abs(prev.z - s.z) >= minDelta;
        if (heartbeatDue || movedEnough) {
            pub.publishPosition(s.objectId, s.x, s.y, s.z, s.instanceId);
            lastPosPublishMs.put(s.objectId, now);
            publishedThisTick++;
        }

        last.put(s.objectId, s);
    }
}
