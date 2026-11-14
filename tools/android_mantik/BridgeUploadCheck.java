package tr.orun.edge;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * PC check: the native bridge's upload against the account server's real
 * batch code (backend/orun_backend_bench.py Backend.batch, the one behind
 * POST /v1/edge/batch), and what the app then takes over (changes()).
 *   1. 300 records stored by BridgeCore -> drain -> every one STORED at the
 *      server with its first arrival, signal and Gateway; nothing pending.
 *   2. Records again from the Gateway: DUPLICATE on the phone, nothing new to send.
 *   3. The server answers UNCERTAIN for some: they stay pending, are not
 *      retried in the same pass, and go in the next one.
 *   4. 401 (session rejected): nothing marked.
 *   5. Sessions reach the server once and again while they grow.
 *   6. Every settled record shows up again in changes() with its state.
 *   7. Prune: old, settled, taken-over rows go; rows with a conflict stay.
 */
public final class BridgeUploadCheck {
    static final String SERVER = String.join("\n",
        "import json, sqlite3, sys",
        "sys.path.insert(0, sys.argv[1])",
        "from backend.orun_backend_bench import Backend",
        "from pathlib import Path",
        "b = Backend(Path(sys.argv[2]), True, clock=lambda: 1800000000)",
        "for line in sys.stdin:",
        "    line = line.strip()",
        "    if line == 'COUNTS':",
        "        db = sqlite3.connect(sys.argv[2])",
        "        r = [db.execute(q).fetchone()[0] for q in (",
        "            'SELECT count(*) FROM edge_receipts WHERE gw_rssi IS NOT NULL',",
        "            'SELECT count(*) FROM backend_uploads WHERE gateway IS NOT NULL',",
        "            'SELECT count(*) FROM edge_sessions',",
        "            'SELECT coalesce(max(alive_utc),0) FROM edge_sessions')]",
        "        print('COUNTS ' + ','.join(str(x) for x in [b.ledger.record_count()] + r)); sys.stdout.flush(); continue",
        "    try:",
        "        out = b.batch(json.loads(bytes.fromhex(line).decode()))",
        "        print('OK ' + ','.join(out['results']) + '|' + ('1' if out['sessions_ok'] else '0'))",
        "    except Exception as e:",
        "        print('ERR ' + type(e).__name__ + ' ' + str(e).replace(chr(10), ' '))",
        "    sys.stdout.flush()");

    static Process proc;
    static Writer in;
    static BufferedReader out;

    static String ask(String line) throws Exception {
        in.write(line + "\n");
        in.flush();
        return out.readLine();
    }

    static String hexUtf8(String s) {
        StringBuilder b = new StringBuilder();
        for (byte v : s.getBytes(StandardCharsets.UTF_8)) b.append(String.format("%02x", v & 0xff));
        return b.toString();
    }

    static int failures;

    static void check(boolean ok, String what) {
        if (!ok) { failures++; System.out.println("FAIL " + what); }
    }

    public static void main(String[] args) throws Exception {
        Path dir = Files.createTempDirectory("bridge-upload");
        proc = new ProcessBuilder("python3", "-I", "-c", SERVER, args[1], dir.resolve("server.sqlite").toString())
                .redirectErrorStream(true).start();
        in = new OutputStreamWriter(proc.getOutputStream(), StandardCharsets.UTF_8);
        out = new BufferedReader(new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8));

        long epoch = 0; byte[] traffic = null; long gw = 0;
        List<byte[]> frames = new ArrayList<>();
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            String[] f = line.split("\\|", -1);
            if (f[0].equals("key")) { epoch = Long.parseLong(f[1]); traffic = BridgeWireCheck.hex(f[2]); gw = Long.parseUnsignedLong(f[4], 16); }
            if (f[0].equals("frame")) frames.add(BridgeWireCheck.hex(f[1]));
        }
        final long ep = epoch; final byte[] tk = traffic;
        ProductFrame.Keys keys = (d, e) -> e == ep ? tk : null;
        PySql sql = new PySql(dir.resolve("bridge.db").toString());
        BridgeStore store = new BridgeStore(sql);
        final long[] now = {1_800_000_000L};
        int i = 0;
        for (byte[] f : frames) {
            ProductFrame.Opened it = ProductFrame.open(f, keys);
            store.put(new BridgeSpool.Arrival(it, gw, -70 - (i % 30), i % 10, now[0] + i), keys);
            i++;
        }
        store.sessionAlive(now[0], "ble");
        store.sessionAlive(now[0] + 600, "ble");
        BridgeUpload.Post server = body -> {
            String r = ask(hexUtf8(body));
            if (r == null || !r.startsWith("OK ")) throw new IllegalStateException("server: " + r);
            String[] p = r.substring(3).split("\\|");
            return new BridgeUpload.Reply(200, p[0].isEmpty() ? new ArrayList<>() : Arrays.asList(p[0].split(",")), p[1].equals("1"));
        };

        // 1. everything goes, newest first, 100 per request
        BridgeUpload.Result r = BridgeUpload.drain(store, server, now[0] + 1000);
        check(r.sent == 300 && r.settled == 300, "1 drain sent " + r.sent + " settled " + r.settled);
        BridgeStore.Counts c = store.counts();
        check(c.pending == 0 && c.uploaded == 300, "1 counts pending " + c.pending + " uploaded " + c.uploaded);
        String[] sc = ask("COUNTS").substring(7).split(",");
        check(sc[0].equals("300"), "1 server records " + sc[0]);
        check(sc[1].equals("300"), "1 server receipts with signal " + sc[1]);
        check(sc[2].equals("300"), "1 server rows with gateway " + sc[2]);
        check(sc[3].equals("1") && sc[4].equals(String.valueOf(now[0] + 600)), "1 server sessions " + sc[3] + " alive " + sc[4]);

        // 2. the Gateway sends them again: duplicates, nothing to upload
        for (byte[] f : frames.subList(0, 50)) {
            String st = store.put(new BridgeSpool.Arrival(ProductFrame.open(f, keys), gw, null, null, now[0] + 5000), keys);
            check(st.equals("DUPLICATE"), "2 again " + st);
        }
        r = BridgeUpload.drain(store, server, now[0] + 6000);
        check(r.sent == 0, "2 nothing to send, sent " + r.sent);

        // 5. a growing session is sent again
        store.sessionAlive(now[0] + 900, "ble");
        r = BridgeUpload.drain(store, server, now[0] + 6000);
        sc = ask("COUNTS").substring(7).split(",");
        check(r.sent == 0 && sc[3].equals("1") && sc[4].equals(String.valueOf(now[0] + 900)), "5 session alive " + sc[4]);
        store.sessionEnd(now[0] + 950);
        store.sessionAlive(now[0] + 2000, "ble");
        BridgeUpload.drain(store, server, now[0] + 6000);
        sc = ask("COUNTS").substring(7).split(",");
        check(sc[3].equals("2"), "5 second session " + sc[3]);

        // 3. fresh store; the server is unsure of every third record
        PySql sql2 = new PySql(dir.resolve("bridge2.db").toString());
        BridgeStore s2 = new BridgeStore(sql2);
        for (byte[] f : frames.subList(0, 250))
            s2.put(new BridgeSpool.Arrival(ProductFrame.open(f, keys), gw, null, null, now[0]), keys);
        final int[] posts = {0};
        BridgeUpload.Post unsure = body -> {
            posts[0]++;
            BridgeUpload.Reply real = server.post(body);
            List<String> words = new ArrayList<>(real.results);
            for (int k = 0; k < words.size(); k += 3) words.set(k, "UNCERTAIN");
            return new BridgeUpload.Reply(200, words, real.sessionsOk);
        };
        r = BridgeUpload.drain(s2, unsure, now[0]);
        check(posts[0] == 3 && r.sent == 250 && r.uncertain == 85, "3 first pass posts " + posts[0] + " sent " + r.sent + " uncertain " + r.uncertain);
        check(s2.counts().pending == 85, "3 pending " + s2.counts().pending);
        r = BridgeUpload.drain(s2, server, now[0]);
        check(r.sent == 85 && r.settled == 85 && s2.counts().pending == 0, "3 second pass sent " + r.sent);

        // 4. session rejected: nothing marked
        PySql sql3 = new PySql(dir.resolve("bridge3.db").toString());
        BridgeStore s3 = new BridgeStore(sql3);
        for (byte[] f : frames.subList(0, 10))
            s3.put(new BridgeSpool.Arrival(ProductFrame.open(f, keys), gw, null, null, now[0]), keys);
        boolean threw = false;
        try { BridgeUpload.drain(s3, body -> new BridgeUpload.Reply(401, null, false), now[0]); }
        catch (BridgeUpload.Unauthorized e) { threw = true; }
        check(threw && s3.counts().pending == 10, "4 401 pending " + s3.counts().pending);
        threw = false;
        try { BridgeUpload.drain(s3, body -> new BridgeUpload.Reply(200, Arrays.asList("STORED"), true), now[0]); }
        catch (IllegalStateException e) { threw = true; }
        check(threw && s3.counts().pending == 10, "4 short answer pending " + s3.counts().pending);

        // 6. the app sees every record, then each again with its upload state
        List<BridgeStore.Change> all = new ArrayList<>();
        long after = 0;
        while (true) {
            List<BridgeStore.Change> ch = store.changes(after, 77);
            if (ch.isEmpty()) break;
            all.addAll(ch);
            after = ch.get(ch.size() - 1).rev;
        }
        long withState = all.stream().filter(x -> "STORED".equals(x.upload)).count();
        check(all.size() == 300 && withState == 300, "6 changes " + all.size() + " with state " + withState);
        for (int k = 1; k < all.size(); k++) check(all.get(k).rev > all.get(k - 1).rev, "6 rev order");
        check(all.stream().allMatch(x -> x.firstSeen != null && x.rssi != null && x.gateway != null && !x.conflict),
              "6 receipt with every change");

        // 7. prune: nothing before the app took it over; then old settled rows go
        check(store.prune(now[0] + BridgeStore.KEEP_S + 10_000) == 0, "7 nothing pruned before takeover");
        store.takenUpTo(after);
        // one conflict (other content, same identity) keeps that identity
        ProductFrame.Opened first = ProductFrame.open(frames.get(0), keys);
        check(store.put(new BridgeSpool.Arrival(ProductFrame.open(frames.get(0), keys), gw, null, null, now[0]), keys).equals("DUPLICATE"), "7 dup");
        sql.exec("INSERT INTO conflicts (device_id,incarnation,sequence,family,plaintext,frame,rev) VALUES (?,?,?,?,?,?,?)",
                 BridgeCore.hexOf(first.device), BridgeCore.hexOf(first.incarnation), first.sequence, 1,
                 "00".repeat(48), "00".repeat(84), 999999);
        int pruned = store.prune(now[0] + BridgeStore.KEEP_S + 10_000);
        check(pruned == 299 && store.counts().records == 1, "7 pruned " + pruned + " left " + store.counts().records);

        sql.close(); sql2.close(); sql3.close(); proc.destroy();
        System.out.println("BRIDGE-UPLOAD " + (failures == 0 ? "PASS" : "FAIL (" + failures + ")"));
        if (failures != 0) System.exit(1);
    }
}
