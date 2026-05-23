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
    // Group voice (party/clan/ally) — resolved lazily, may be null if
    // the L2J build doesn't expose them via these exact names.
    private final Method getParty;           // L2PcInstance.getParty() -> L2Party
    private final Method getPartyMembers;    // L2Party.getPartyMembers() -> List<L2PcInstance>
    private final Method getClan;            // L2PcInstance.getClan() -> L2Clan
    private final Method getOnlineMembersList; // L2Clan.getOnlineMembersList()
    private final Method getAllyId;          // L2Clan.getAllyId() -> int

    // Extra L2J accessors used by the clan-voice expansion.
    // Resolved best-effort — missing ones leave the corresponding
    // feature off rather than aborting init.
    private Method getLeaderId;       // L2Clan.getLeaderId()

    // Command Channel (CC) — L2Party.getCommandChannel() returns an
    // L2CommandChannel; the CC has a leader (L2PcInstance) and a list
    // of L2Partys. All resolved by-name with reflection-tolerant
    // lookup so missing methods just disable CC voice gracefully.
    private Method getCommandChannel;       // L2Party.getCommandChannel()
    private Method ccGetChannelLeader;      // L2CommandChannel.getChannelLeader()
    private Method ccGetPartys;             // L2CommandChannel.getPartys() / getPartyMembers()

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

        // Optional methods — resolved with reflection-tolerant
        // lookup so a missing one doesn't abort init.
        getParty               = tryGetMethod(pc, "getParty");
        getClan                = tryGetMethod(pc, "getClan");
        Class<?> party = tryClass("net.l2emuproject.gameserver.model.L2Party");
        getPartyMembers        = party == null ? null : tryGetMethod(party, "getPartyMembers");
        Class<?> clan  = tryClass("net.l2emuproject.gameserver.model.L2Clan");
        getOnlineMembersList   = clan == null ? null : tryGetMethod(clan, "getOnlineMembersList");
        getAllyId              = clan == null ? null : tryGetMethod(clan, "getAllyId");
        getLeaderId            = clan == null ? null : tryGetMethod(clan, "getLeaderId");
        getCommandChannel      = party == null ? null : tryGetMethod(party, "getCommandChannel");
        Class<?> cc = tryClass("net.l2emuproject.gameserver.model.L2CommandChannel");
        ccGetChannelLeader     = cc == null ? null : tryGetMethod(cc, "getChannelLeader");
        // L2J builds vary: some use getPartys(), some getPartyMembers().
        ccGetPartys            = cc == null ? null : tryGetMethod(cc, "getPartys");
        if (ccGetPartys == null && cc != null) {
            ccGetPartys = tryGetMethod(cc, "getPartyMembers");
        }

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

    @FunctionalInterface
    interface SnapshotSink {
        void accept(PlayerSnapshot s);
    }

    /** Iterates all online players and feeds a full snapshot to the
     *  sink. Used by PositionPoller to diff per-player state and emit
     *  change events (clan / ally / party / position). */
    void forEachSnapshot(SnapshotSink sink) throws Exception {
        Object res = getAllPlayers.invoke(null);
        Iterable<?> iter;
        if (res instanceof Iterable<?> it)     iter = it;
        else if (res instanceof Object[] arr)  iter = java.util.Arrays.asList(arr);
        else if (res instanceof java.util.Map<?, ?> m) iter = m.values();
        else                                   return;
        for (Object p : iter) {
            if (p == null) continue;
            try { sink.accept(snapshot(p)); } catch (Throwable ignored) {}
        }
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
            // IP matching rules:
            //  - If the L2GameClient is loopback, the L2 client lives on
            //    the same machine as the GS. The voice-server (which may
            //    be remote, e.g. on a VPS) sees the WS connection from
            //    the user's PUBLIC IP, which won't match 127.0.0.1. The
            //    port number is unique per active TCP socket on this
            //    machine, so the port match alone is sufficient.
            //  - Otherwise (production: GS on its own host, L2 clients
            //    on player PCs), require exact IP match.
            boolean ipOk;
            if (isLoopback(host)) {
                ipOk = true;
            } else {
                ipOk = host.equals(clientIp);
            }
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

    /** Snapshot of the L2J state for a single player. Cheap value
     *  object — PositionPoller builds one per online player per tick
     *  and diffs against the previous to emit change events. */
    static final class PlayerSnapshot {
        int objectId;
        int x, y, z, instanceId;
        int clanId;          // 0 = no clan
        int allyId;          // 0 = no alliance
        int partyId;         // 0 = no party; otherwise identityHashCode(L2Party)
        boolean isClanLeader;

        // Command Channel (CC). All 0 / empty when player is not in a CC.
        int ccId;            // identityHashCode(L2CommandChannel) & 0x7FFFFFFF
        int ccLeaderId;
        int[] ccMembers;     // all object-ids in the CC, including the leader
    }

    /** Reads everything Phase-A cares about from one L2PcInstance. */
    PlayerSnapshot snapshot(Object pc) throws Exception {
        PlayerSnapshot s = new PlayerSnapshot();
        s.objectId   = (int) getObjectId.invoke(pc);
        s.x          = (int) getX.invoke(pc);
        s.y          = (int) getY.invoke(pc);
        s.z          = (int) getZ.invoke(pc);
        s.instanceId = (int) getInstanceId.invoke(pc);
        Object clan = (getClan != null) ? getClan.invoke(pc) : null;
        if (clan != null) {
            // L2Clan.getId — fall through to any int-returning method
            // named getClanId / getId; common in L2J forks.
            int cid = invokeIntOrZero(clan, "getId");
            if (cid == 0) cid = invokeIntOrZero(clan, "getClanId");
            s.clanId = cid;
            s.allyId = getAllyId != null ? (int) getAllyId.invoke(clan) : 0;
            if (getLeaderId != null) {
                s.isClanLeader = ((int) getLeaderId.invoke(clan)) == s.objectId;
            }
        }
        Object party = (getParty != null) ? getParty.invoke(pc) : null;
        if (party != null) {
            // No stable id in L2Party — use object identity. Stable
            // within a single bridge JVM lifetime; resets on restart
            // (voice-service caches will recover within ~5 s polls).
            // Mask off the sign bit so JSON serialization stays positive;
            // the Go side unmarshals into uint64 and rejects negatives.
            s.partyId = System.identityHashCode(party) & 0x7FFFFFFF;

            // Command Channel — only L2Party knows about it.
            if (getCommandChannel != null) {
                Object commandChannel = getCommandChannel.invoke(party);
                if (commandChannel != null) {
                    s.ccId = System.identityHashCode(commandChannel) & 0x7FFFFFFF;
                    if (ccGetChannelLeader != null) {
                        Object leader = ccGetChannelLeader.invoke(commandChannel);
                        if (leader != null) {
                            s.ccLeaderId = (int) getObjectId.invoke(leader);
                        }
                    }
                    s.ccMembers = collectCCMemberIds(commandChannel);
                }
            }
        }
        return s;
    }

    /** Walks all L2Partys inside the CC and collects every member's
     *  object-id, including the channel leader. Empty array if the
     *  L2J build doesn't expose getPartys()/getPartyMembers(). */
    private int[] collectCCMemberIds(Object commandChannel) throws Exception {
        if (ccGetPartys == null || getPartyMembers == null) return new int[0];
        Object partys = ccGetPartys.invoke(commandChannel);
        if (partys == null) return new int[0];
        java.util.List<Integer> ids = new java.util.ArrayList<>();
        Iterable<?> iter;
        if (partys instanceof Iterable<?> it)        iter = it;
        else if (partys instanceof Object[] arr)     iter = java.util.Arrays.asList(arr);
        else if (partys instanceof java.util.Map<?,?> m) iter = m.values();
        else return new int[0];
        for (Object p : iter) {
            if (p == null) continue;
            Object members = getPartyMembers.invoke(p);
            if (members == null) continue;
            Iterable<?> mIter;
            if (members instanceof Iterable<?> mi)        mIter = mi;
            else if (members instanceof Object[] arr)     mIter = java.util.Arrays.asList(arr);
            else continue;
            for (Object pc : mIter) {
                if (pc != null) ids.add((int) getObjectId.invoke(pc));
            }
        }
        int[] out = new int[ids.size()];
        for (int i = 0; i < out.length; i++) out[i] = ids.get(i);
        return out;
    }

    private static int invokeIntOrZero(Object target, String methodName) {
        try {
            Method m = target.getClass().getMethod(methodName);
            return (int) m.invoke(target);
        } catch (Throwable t) {
            return 0;
        }
    }

    private static Method tryGetMethod(Class<?> c, String name) {
        try { return c.getMethod(name); }
        catch (Throwable t) { return null; }
    }
    private static Class<?> tryClass(String fqn) {
        try { return Class.forName(fqn); }
        catch (Throwable t) { return null; }
    }

    /**
     * Returns the list of object-ids that belong to the same voice
     * group as {@code objId} for the given channel.
     *   channel=1 → party    (L2PcInstance.getParty().getPartyMembers())
     *   channel=2 → clan     (L2PcInstance.getClan().getOnlineMembersList())
     *   channel=3 → ally     (all online players in any clan sharing
     *                         this player's clan's allyId)
     * The returned list INCLUDES the asking player. Caller may want
     * to filter the speaker out.
     */
    int[] getGroupMembers(int objId, int channel) throws Exception {
        Object res = getAllPlayers.invoke(null);
        Iterable<?> iter;
        if (res instanceof Iterable<?> it)     iter = it;
        else if (res instanceof Object[] arr)  iter = java.util.Arrays.asList(arr);
        else if (res instanceof java.util.Map<?, ?> m) iter = m.values();
        else                                   return new int[0];

        // Find the asking player first.
        Object asking = null;
        for (Object p : iter) {
            if (p != null && (int) getObjectId.invoke(p) == objId) {
                asking = p; break;
            }
        }
        if (asking == null) return new int[0];

        switch (channel) {
            case 1: { // party
                if (getParty == null || getPartyMembers == null) return new int[0];
                Object party = getParty.invoke(asking);
                if (party == null) return new int[0];
                Object list = getPartyMembers.invoke(party);
                return toIdArray(list);
            }
            case 2: { // clan
                if (getClan == null || getOnlineMembersList == null) return new int[0];
                Object clan = getClan.invoke(asking);
                if (clan == null) return new int[0];
                Object list = getOnlineMembersList.invoke(clan);
                return toIdArray(list);
            }
            case 3: { // ally
                if (getClan == null || getAllyId == null) return new int[0];
                Object clan = getClan.invoke(asking);
                if (clan == null) return new int[0];
                int allyId = (int) getAllyId.invoke(clan);
                if (allyId == 0) return new int[0];
                // Walk all online players, filter by their clan's allyId.
                java.util.List<Integer> out = new java.util.ArrayList<>();
                for (Object p : iter) {
                    if (p == null) continue;
                    Object pc = getClan.invoke(p);
                    if (pc == null) continue;
                    int aid = (int) getAllyId.invoke(pc);
                    if (aid == allyId) out.add((int) getObjectId.invoke(p));
                }
                int[] arr = new int[out.size()];
                for (int i = 0; i < arr.length; i++) arr[i] = out.get(i);
                return arr;
            }
            default: return new int[0];
        }
    }

    private int[] toIdArray(Object collection) throws Exception {
        java.util.List<Integer> out = new java.util.ArrayList<>();
        Iterable<?> iter;
        if (collection instanceof Iterable<?> it) iter = it;
        else if (collection instanceof Object[] arr) iter = java.util.Arrays.asList(arr);
        else return new int[0];
        for (Object p : iter) {
            if (p != null) out.add((int) getObjectId.invoke(p));
        }
        int[] arr = new int[out.size()];
        for (int i = 0; i < arr.length; i++) arr[i] = out.get(i);
        return arr;
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
