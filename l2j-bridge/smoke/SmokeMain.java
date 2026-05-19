// SmokeMain — start the bridge with a config, sleep, stop. Used to
// verify the jar initializes cleanly without requiring the GS or
// Redis to actually be running. Compile/run:
//
//   javac -cp target/l2voice-bridge-0.1.0.jar smoke/SmokeMain.java
//   java -cp "target/l2voice-bridge-0.1.0.jar;smoke" SmokeMain
//
// Expected behavior: prints "l2voice bridge started", logs Redis
// connect-retry warnings (Redis not present), then "l2voice bridge
// stopped". HTTP server should bind on 17668 successfully.
import java.util.Properties;

public class SmokeMain {
    public static void main(String[] args) throws Exception {
        Properties p = new Properties();
        p.setProperty("l2voice.hmac.secret", "smoke-secret-12345");
        p.setProperty("l2voice.redis.host",  "127.0.0.1");
        p.setProperty("l2voice.redis.port",  "6379");
        p.setProperty("l2voice.http.bind",   "127.0.0.1");
        p.setProperty("l2voice.http.port",   "17668");
        p.setProperty("l2voice.position.hz", "5");

        com.luannbr.l2voice.bridge.VoiceBridge.start(p);
        Thread.sleep(5_000);

        // Mint a token; verify it's well-formed.
        Object t = com.luannbr.l2voice.bridge.VoiceBridge
                       .issueTokenOnLogin(1234567);
        System.out.println("smoke: token = " + t);

        com.luannbr.l2voice.bridge.VoiceBridge.stop();
    }
}
