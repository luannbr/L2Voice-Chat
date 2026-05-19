package com.luannbr.l2voice.bridge;

import java.lang.reflect.Method;

/**
 * Reflection facade for L2J's {@code L2World} (Essence 542).
 *
 * <p>Bound to {@code net.l2emuproject.gameserver.model.L2World} static
 * {@code getAllPlayers()} and the {@code L2PcInstance} field getters.
 *
 * <p>Throws at construction if the GS classes aren't on the classpath
 * — callers catch and disable features that depend on it.
 */
final class L2WorldRef {

    private final Method getAllPlayers;     // static
    private final Method getObjectId;
    private final Method getX, getY, getZ;
    private final Method getInstanceId;

    L2WorldRef() throws Exception {
        Class<?> world = Class.forName("net.l2emuproject.gameserver.model.L2World");
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
        Object res = getAllPlayers.invoke(null);
        if (res instanceof Iterable<?> iter) {
            for (Object p : iter) snapshot(p, sink);
        } else if (res instanceof Object[] arr) {
            for (Object p : arr) snapshot(p, sink);
        } else if (res instanceof java.util.Map<?, ?> map) {
            for (Object p : map.values()) snapshot(p, sink);
        }
    }

    boolean containsPlayer(int objId) throws Exception {
        Object res = getAllPlayers.invoke(null);
        if (res instanceof Iterable<?> iter) {
            for (Object p : iter) {
                if (p != null && ((int) getObjectId.invoke(p)) == objId) return true;
            }
        } else if (res instanceof Object[] arr) {
            for (Object p : arr) {
                if (p != null && ((int) getObjectId.invoke(p)) == objId) return true;
            }
        } else if (res instanceof java.util.Map<?, ?> map) {
            for (Object p : map.values()) {
                if (p != null && ((int) getObjectId.invoke(p)) == objId) return true;
            }
        }
        return false;
    }

    private void snapshot(Object p, Sink sink) throws Exception {
        if (p == null) return;
        int oid  = (int) getObjectId.invoke(p);
        int x    = (int) getX.invoke(p);
        int y    = (int) getY.invoke(p);
        int z    = (int) getZ.invoke(p);
        int inst = (int) getInstanceId.invoke(p);
        sink.accept(oid, x, y, z, inst);
    }
}
