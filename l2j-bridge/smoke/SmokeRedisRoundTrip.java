// SmokeRedisRoundTrip — boots the bridge AND a Jedis subscriber on the
// same channel in the same JVM. Drives the bridge to publish a few
// events, then prints whatever was actually delivered.
//
//   javac -cp target/l2voice-bridge-0.1.0.jar smoke/SmokeRedisRoundTrip.java
//   java  -cp "target/l2voice-bridge-0.1.0.jar;smoke" SmokeRedisRoundTrip
//
// Exit code 0 if at least one event was received; non-zero otherwise.
import java.util.Properties;
import java.util.concurrent.atomic.AtomicInteger;
import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisPubSub;

public class SmokeRedisRoundTrip {
    public static void main(String[] args) throws Exception {
        Properties p = new Properties();
        p.setProperty("l2voice.hmac.secret", "smoke-secret-12345");
        p.setProperty("l2voice.redis.host",  "127.0.0.1");
        p.setProperty("l2voice.redis.port",  "6379");
        p.setProperty("l2voice.http.bind",   "127.0.0.1");
        p.setProperty("l2voice.http.port",   "17668");

        AtomicInteger received = new AtomicInteger();
        Thread sub = new Thread(() -> {
            try (Jedis j = new Jedis("127.0.0.1", 6379)) {
                j.subscribe(new JedisPubSub() {
                    @Override public void onMessage(String chan, String msg) {
                        System.out.println("[recv] " + chan + " -> " + msg);
                        received.incrementAndGet();
                    }
                }, "l2voice:events");
            }
        }, "subscriber");
        sub.setDaemon(true);
        sub.start();
        Thread.sleep(500);  // let subscribe settle

        com.luannbr.l2voice.bridge.VoiceBridge.start(p);
        Thread.sleep(300);  // let publisher worker connect

        // Drive a few events through the public API.
        com.luannbr.l2voice.bridge.VoiceBridge.issueTokenOnLogin(1234567);
        com.luannbr.l2voice.bridge.VoiceBridge.notifyLogout(1234567);

        Thread.sleep(1000);
        com.luannbr.l2voice.bridge.VoiceBridge.stop();

        System.out.println("[smoke] events received: " + received.get());
        System.exit(received.get() >= 2 ? 0 : 1);
    }
}
