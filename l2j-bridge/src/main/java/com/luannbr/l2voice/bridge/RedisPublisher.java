package com.luannbr.l2voice.bridge;

import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.logging.Level;
import java.util.logging.Logger;

import redis.clients.jedis.Jedis;

/**
 * Publishes JSON events on a Redis pub/sub channel. A single worker
 * thread owns the Jedis connection; producers enqueue serialized JSON
 * onto a bounded queue. If the queue fills (Redis unreachable, etc.)
 * the oldest events are dropped — voice is best-effort.
 *
 * <p>Wire format and event vocabulary: see {@code docs/protocol.md} §5.
 */
final class RedisPublisher {

    private static final Logger log = Logger.getLogger(RedisPublisher.class.getName());

    private final String host;
    private final int    port;
    private final String channel;

    private final BlockingQueue<String> queue = new ArrayBlockingQueue<>(4096);
    private volatile boolean stopping = false;
    private Thread worker;

    RedisPublisher(String host, int port, String channel) {
        this.host    = host;
        this.port    = port;
        this.channel = channel;
    }

    void start() {
        worker = new Thread(this::run, "l2voice-redis-pub");
        worker.setDaemon(true);
        worker.start();
    }

    void stop() {
        stopping = true;
        if (worker != null) worker.interrupt();
    }

    private void run() {
        while (!stopping) {
            try (Jedis j = new Jedis(host, port)) {
                log.info("connected to redis " + host + ":" + port);
                while (!stopping) {
                    String msg = queue.poll(1, TimeUnit.SECONDS);
                    if (msg == null) continue;
                    j.publish(channel, msg);
                }
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
                return;
            } catch (Exception e) {
                log.log(Level.WARNING, "redis publisher loop failed, reconnecting", e);
                try { Thread.sleep(2_000); }
                catch (InterruptedException ie) { return; }
            }
        }
    }

    // ---- event emitters ------------------------------------------------

    void publishPosition(int playerId, int x, int y, int z, int instanceId) {
        offer(envelope(playerId, "position",
                "\"x\":" + x + ",\"y\":" + y + ",\"z\":" + z +
                ",\"instance_id\":" + instanceId));
    }

    void publishPlayerLogin(int playerId) {
        offer(envelope(playerId, "player_login", ""));
    }

    void publishPlayerLogout(int playerId) {
        offer(envelope(playerId, "player_logout", ""));
    }

    void publishInstanceChange(int playerId, int instanceId) {
        offer(envelope(playerId, "instance_change",
                "\"instance_id\":" + instanceId));
    }

    /** Clan id changed (joined a clan, left, switched). 0 = no clan. */
    void publishClanChange(int playerId, int clanId, boolean isLeader) {
        offer(envelope(playerId, "clan_change",
                "\"clan_id\":" + clanId +
                ",\"is_leader\":" + isLeader));
    }

    /** Ally id changed. 0 = no alliance. */
    void publishAllyChange(int playerId, int allyId) {
        offer(envelope(playerId, "ally_change",
                "\"ally_id\":" + allyId));
    }

    /** Party identity changed. partyId = 0 means the player left their
     *  party; otherwise an opaque stable id for the duration of the
     *  party (within this bridge process). */
    void publishPartyChange(int playerId, int partyId) {
        offer(envelope(playerId, "party_change",
                "\"party_id\":" + partyId));
    }

    /** A clan's leader changed — emitted with the new leader's id. */
    void publishClanLeaderChange(int clanId, int newLeaderId) {
        offer(envelope(newLeaderId, "clan_leader_change",
                "\"clan_id\":" + clanId +
                ",\"leader_id\":" + newLeaderId));
    }

    /** Command-channel state for one player. ccId=0 means the player
     *  is no longer in any CC; members may still be empty even when
     *  ccId > 0 (which simply means the bridge couldn't enumerate
     *  members — voice-service treats it as a request to delete the
     *  CC silently). */
    void publishCCChange(int playerId, int ccId, int ccLeaderId, int[] members) {
        StringBuilder sb = new StringBuilder(96);
        sb.append("\"cc_id\":").append(ccId)
          .append(",\"cc_leader_id\":").append(ccLeaderId)
          .append(",\"members\":[");
        if (members != null) {
            for (int i = 0; i < members.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(members[i]);
            }
        }
        sb.append(']');
        offer(envelope(playerId, "cc_change", sb.toString()));
    }

    // ---- internals -----------------------------------------------------

    private static String envelope(int playerId, String event, String dataBody) {
        return "{\"ts\":" + System.currentTimeMillis() +
                ",\"event\":\"" + event + "\"" +
                ",\"player_id\":" + playerId +
                ",\"data\":{" + dataBody + "}}";
    }

    private void offer(String msg) {
        // Phase B: fan the event out to every connected voice-server
        // WS link in parallel with the Redis publish path. Both
        // transports carry the same envelope; the voice-server picks
        // whichever arrives (idempotent state mutations).
        VoiceServerLink.broadcastEvent(msg);
        // Drop oldest when full — voice events are perishable.
        while (!queue.offer(msg)) {
            queue.poll();
        }
    }
}
