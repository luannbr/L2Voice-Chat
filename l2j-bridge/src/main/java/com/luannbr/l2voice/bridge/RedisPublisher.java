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

    // ---- internals -----------------------------------------------------

    private static String envelope(int playerId, String event, String dataBody) {
        return "{\"ts\":" + System.currentTimeMillis() +
                ",\"event\":\"" + event + "\"" +
                ",\"player_id\":" + playerId +
                ",\"data\":{" + dataBody + "}}";
    }

    private void offer(String msg) {
        // Drop oldest when full — voice events are perishable.
        while (!queue.offer(msg)) {
            queue.poll();
        }
    }
}
