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

    // For TCP-port identity matching.
    private final Method getClient;          // L2PcInstance.getClient()
    private final Method getConnection;      // L2GameClient.getConnection()
    private final Method getSocketChannel;   // MMOConnection.getSocketChannel()
    private final Method getName;            // L2PcInstance.getName()

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
        getClient     = pc.getMethod("getClient");
        getName       = pc.getMethod("getName");

        Class<?> gameClient = Class.forName(
                "net.l2emuproject.gameserver.network.L2GameClient");
        getConnection = gameClient.getMethod("getConnection");

        Class<?> mmo = Class.forName("org.mmocore.network.MMOConnection");
        getSocketChannel = mmo.getMethod("getSocketChannel");
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

    /**
     * Finds the player whose game-client TCP connection comes from the
     * given {@code clientIp} and has a remote (source) port in
     * {@code candidatePorts}. Returns the player's object id, or 0 if
     * no match.
     *
     * <p>Why this works: each L2.exe opens one TCP socket to the GS.
     * The DLL enumerates its own process's TCP table and ships the
     * local-port list to the voice-service. From the GS's perspective,
     * those local ports ARE the remote ports of its L2GameClient
     * sockets. Same TCP four-tuple seen from opposite sides.
     */
    int findPlayerByConnection(String clientIp, java.util.Set<Integer> candidatePorts)
            throws Exception {
        Object res = getAllPlayers.invoke(null);
        Iterable<?> iter;
        if (res instanceof Iterable<?> it)     iter = it;
        else if (res instanceof Object[] arr)  iter = java.util.Arrays.asList(arr);
        else if (res instanceof java.util.Map<?, ?> m) iter = m.values();
        else                                   return 0;

        for (Object p : iter) {
            if (p == null) continue;
            Object client = getClient.invoke(p);
            if (client == null) continue;
            Object conn = getConnection.invoke(client);
            if (conn == null) continue;
            Object sc = getSocketChannel.invoke(conn);
            if (!(sc instanceof java.nio.channels.SocketChannel ch)) continue;
            java.net.Socket sk = ch.socket();
            if (sk == null) continue;
            java.net.InetAddress addr = sk.getInetAddress();
            int port = sk.getPort();
            if (addr == null) continue;
            String host = addr.getHostAddress();
            // Accept exact IP match OR localhost equivalence.
            boolean ipOk = host.equals(clientIp)
                    || (isLoopback(host) && isLoopback(clientIp));
            if (!ipOk) continue;
            if (!candidatePorts.contains(port)) continue;
            return (int) getObjectId.invoke(p);
        }
        return 0;
    }

    private static boolean isLoopback(String s) {
        return s.equals("127.0.0.1") || s.equals("0:0:0:0:0:0:0:1")
                || s.equals("::1") || s.equalsIgnoreCase("localhost");
    }

    /** Returns the character name for an online player, or null. */
    String getPlayerName(int objId) throws Exception {
        Object res = getAllPlayers.invoke(null);
        Iterable<?> iter;
        if (res instanceof Iterable<?> it)     iter = it;
        else if (res instanceof Object[] arr)  iter = java.util.Arrays.asList(arr);
        else if (res instanceof java.util.Map<?, ?> m) iter = m.values();
        else                                   return null;
        for (Object p : iter) {
            if (p == null) continue;
            if ((int) getObjectId.invoke(p) == objId) {
                Object name = getName.invoke(p);
                return name == null ? null : name.toString();
            }
        }
        return null;
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
