package com.luannbr.l2voice.bridge;

import java.lang.reflect.Method;
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
        L2WorldReflector world;
        try {
            world = new L2WorldReflector();
        } catch (Exception e) {
            log.log(Level.SEVERE, "could not bind L2World via reflection; poller disabled", e);
            return;
        }
        while (!stopping.get()) {
            long t0 = System.currentTimeMillis();
            try {
                world.forEachPlayer(this::tickPlayer);
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

    private void tickPlayer(int objectId, int x, int y, int z, int instanceId) {
        int[] prev = last.get(objectId);
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
    }

    /**
     * Reflection helper. Bound to the Essence 542 class layout:
     *   {@code net.l2emuproject.gameserver.world.L2World#getAllPlayers()}
     *   {@code net.l2emuproject.gameserver.model.actor.instance.L2PcInstance}
     *   — getObjectId(), getX(), getY(), getZ(), getInstanceId()
     */
    private static final class L2WorldReflector {
        private final Object       worldInstance;
        private final Method       getAllPlayers;
        private final Method       getObjectId;
        private final Method       getX, getY, getZ;
        private final Method       getInstanceId;

        L2WorldReflector() throws Exception {
            Class<?> world = Class.forName("net.l2emuproject.gameserver.world.L2World");
            // getInstance() (singleton)
            worldInstance = world.getMethod("getInstance").invoke(null);
            getAllPlayers = world.getMethod("getAllPlayers");

            Class<?> pc = Class.forName(
                    "net.l2emuproject.gameserver.model.actor.instance.L2PcInstance");
            getObjectId   = pc.getMethod("getObjectId");
            getX          = pc.getMethod("getX");
            getY          = pc.getMethod("getY");
            getZ          = pc.getMethod("getZ");
            getInstanceId = pc.getMethod("getInstanceId");
        }

        @FunctionalInterface
        interface Sink {
            void accept(int oid, int x, int y, int z, int instanceId);
        }

        void forEachPlayer(Sink sink) throws Exception {
            Object res = getAllPlayers.invoke(worldInstance);
            if (res instanceof Iterable<?> iter) {
                for (Object p : iter) snapshot(p, sink);
            } else if (res instanceof Object[] arr) {
                for (Object p : arr) snapshot(p, sink);
            } else if (res instanceof java.util.Map<?, ?> map) {
                for (Object p : map.values()) snapshot(p, sink);
            }
        }

        private void snapshot(Object p, Sink sink) throws Exception {
            if (p == null) return;
            int oid = (int) getObjectId.invoke(p);
            int x   = (int) getX.invoke(p);
            int y   = (int) getY.invoke(p);
            int z   = (int) getZ.invoke(p);
            int inst = (int) getInstanceId.invoke(p);
            sink.accept(oid, x, y, z, inst);
        }
    }
}
