package com.luannbr.l2voice.bridge;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Periodically reads positions of online players from L2World and
 * publishes {@code "position"} events. Throttles per player by a
 * minimum delta to avoid flooding the bus when nothing is moving.
 *
 * <p>We use reflection to avoid a hard compile-time dependency on the
 * L2J class names — the bridge can outlive minor refactors of
 * {@code L2World}/{@code L2PcInstance} that way. The class/method
 * names are L2J Essence 542 specifics; tweak them here if you fork.
 */
final class PositionPoller {

    private static final Logger log = Logger.getLogger(PositionPoller.class.getName());

    private final RedisPublisher pub;
    private final int hz;
    private final int minDelta;

    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private Thread thread;

    /** Last-published snapshot per player to gate by delta. */
    private final Map<Integer, int[]> last = new HashMap<>();

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
                int[] cnt = {0, 0};   // seen, published
                world.forEachPlayer((oid, x, y, z, inst) -> {
                    cnt[0]++;
                    int before = publishedThisTick;
                    tickPlayer(oid, x, y, z, inst);
                    if (publishedThisTick > before) cnt[1]++;
                });
                publishedSinceStats += cnt[1];
                // Stats log every ~30 s so we can see PositionPoller is alive.
                if (t0 - lastStats > 30_000) {
                    log.info("PositionPoller: " + cnt[0] + " players visible, "
                            + publishedSinceStats + " position events in last "
                            + ((t0 - lastStats) / 1000) + "s");
                    lastStats = t0;
                    publishedSinceStats = 0;
                }
            } catch (Exception e) {
                log.log(Level.WARNING, "position poll iteration failed", e);
            }
            long sleep = periodMs - (System.currentTimeMillis() - t0);
            if (sleep > 0) {
                try { Thread.sleep(sleep); }
                catch (InterruptedException ie) { return; }
            }
        }
    }

    private int publishedThisTick = 0;

    private void tickPlayer(int objectId, int x, int y, int z, int instanceId) {
        int[] prev = last.get(objectId);
        // First sight of a player → always publish (voice-service needs a
        // position before it can route any proximity audio for them).
        // After that, throttle by minDelta.
        if (prev != null && Math.abs(prev[0] - x) < minDelta
                         && Math.abs(prev[1] - y) < minDelta
                         && Math.abs(prev[2] - z) < minDelta
                         && prev[3] == instanceId) {
            return;
        }
        if (prev == null) {
            last.put(objectId, new int[]{x, y, z, instanceId});
        } else {
            prev[0] = x; prev[1] = y; prev[2] = z; prev[3] = instanceId;
        }
        pub.publishPosition(objectId, x, y, z, instanceId);
        publishedThisTick++;
    }

}
