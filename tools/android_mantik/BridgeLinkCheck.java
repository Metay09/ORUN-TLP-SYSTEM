package tr.orun.edge;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.Random;
import java.util.Set;

/**
 * PC check of the native bridge as a whole: BridgeLink + BridgeCore +
 * BridgeStore (real SQLite) against a simulated Gateway that behaves like
 * the firmware (sf5l0_bench_gateway.cpp / edge_link): HELLO when the phone
 * turns indications on, every held record once (oldest first), DUMP END,
 * live records as they arrive, ACCEPT checked with the Edge key (BUSY now
 * and then), RECLAIMED when a page is empty, DUMP on request.
 *
 * Each seed is hours of virtual time with link drops, Bluetooth off/on,
 * the Gateway out of range (also for long stretches), busy and failing
 * writes, lost write answers and lost indications. At the end every record
 * must be on the phone exactly once, the Gateway must be empty, and the
 * Gateway must never have handed off a record the phone did not hold.
 */
public final class BridgeLinkCheck {
    // ---- virtual time ----------------------------------------------------------
    static final class Clock implements BridgeLink.Timers {
        long now = 1_800_000_000_000L;
        long seq;
        final PriorityQueue<long[]> due = new PriorityQueue<>((a, b) -> a[0] != b[0] ? Long.compare(a[0], b[0]) : Long.compare(a[1], b[1]));
        final Map<Long, Runnable> jobs = new HashMap<>();
        public long nowMs() { return now; }
        public void after(long ms, Runnable r) { long id = seq++; jobs.put(id, r); due.add(new long[]{now + Math.max(0, ms), id}); }
        void runUntil(long t) {
            while (!due.isEmpty() && due.peek()[0] <= t) {
                long[] d = due.poll();
                now = Math.max(now, d[0]);
                jobs.remove(d[1]).run();
            }
            now = Math.max(now, t);
        }
    }

    // ---- the Gateway -------------------------------------------------------------
    static final class Rec { int page, slot; long gen; byte[] object; String id; }

    static final class Gateway {
        final long id;
        final byte[] edgeKey;
        final Random rnd;
        final Map<String, Rec> held = new LinkedHashMap<>();   // "page:slot", oldest first
        final List<Rec> incoming = new ArrayList<>();
        int nextSlot;
        boolean session;
        final Deque<byte[]> out = new ArrayDeque<>();
        final EdgeWire.Reassembler in = new EdgeWire.Reassembler();
        int msgId;
        final List<String> handedOff = new ArrayList<>();
        final Set<String> everHeld = new HashSet<>();
        int busyReplies, rejected, helloLost;
        BridgeStore store;
        Gateway(long id, byte[] edgeKey, Random rnd) { this.id = id; this.edgeKey = edgeKey; this.rnd = rnd; }

        void receive(Rec r) {                                    // a record arrives over LoRa
            r.page = 2 + nextSlot / 8; r.slot = nextSlot % 8; r.gen = 0x1000 + nextSlot; nextSlot++;
            held.put(r.page + ":" + r.slot, r);
            everHeld.add(r.id);
            if (session) send(held(r, rnd.nextInt(4) == 0 ? 0x06 : 0x02));
        }

        static byte[] be(long v, int n) { byte[] b = new byte[n]; for (int i = n - 1; i >= 0; i--) { b[i] = (byte) v; v >>>= 8; } return b; }

        byte[] held(Rec r, int type) {
            byte[] m = new byte[18 + r.object.length];
            m[0] = (byte) type;
            System.arraycopy(be(r.page, 2), 0, m, 1, 2); System.arraycopy(be(r.slot, 2), 0, m, 3, 2);
            System.arraycopy(be(r.gen, 8), 0, m, 5, 8);
            m[13] = 1; System.arraycopy(be(-80, 2), 0, m, 14, 2); m[16] = 7; m[17] = (byte) r.object.length;
            System.arraycopy(r.object, 0, m, 18, r.object.length);
            return m;
        }

        void send(byte[] message) { msgId = (msgId + 1) & 0xff; out.addAll(EdgeWire.frames(message, msgId)); }

        void begin() {
            session = true; out.clear(); in.reset();
            byte[] h = new byte[13]; h[0] = 1;
            System.arraycopy(be(id, 8), 0, h, 1, 8); System.arraycopy(be(held.size(), 2), 0, h, 9, 2);
            System.arraycopy(be(512, 2), 0, h, 11, 2);
            if (rnd.nextInt(4) == 0) helloLost++;          // the first message lost on the way
            else send(h);
            dump();
        }

        void dump() {
            for (Rec r : held.values()) send(held(r, 0x02));
            send(new byte[]{3, (byte) (held.size() >> 8), (byte) held.size()});
        }

        void end() { session = false; out.clear(); in.reset(); }

        void frameIn(byte[] frame, long now) throws Exception {
            byte[] m = in.push(frame, now);
            if (m == null) return;
            if (m[0] == (byte) 0x81) { dump(); return; }
            if (m[0] == (byte) 0x83) { send(("\u0007LORA QUEUED result=ADDED").getBytes("US-ASCII")); return; }
            if (m[0] != (byte) 0x82 || m.length != 45) return;
            int page = EdgeWire.u16(m, 1), slot = EdgeWire.u16(m, 3);
            long gen = EdgeWire.u64(m, 5);
            byte[] reply = {4, m[1], m[2], m[3], m[4], 0};
            Rec r = held.get(page + ":" + slot);
            if (r == null) { reply[5] = 2; send(reply); return; }       // UNKNOWN (already reclaimed)
            byte[] mac = EdgeWire.acceptMac(id, page, slot, gen, r.object, edgeKey);
            if (gen != r.gen || !Arrays.equals(mac, Arrays.copyOfRange(m, 13, 45))) { reply[5] = 4; rejected++; send(reply); return; }
            if (rnd.nextInt(10) == 0) { reply[5] = 3; busyReplies++; send(reply); return; }   // store busy
            // The firmware's invariant this check exists for: the phone holds it.
            ProductFrame.Opened it = ProductFrame.open(r.object, KEYS);
            if (store.find(it.device, it.incarnation, it.sequence) == null)
                throw new AssertionError("Gateway handed off a record the phone does not hold: " + r.id);
            held.remove(page + ":" + slot);
            handedOff.add(r.id);
            reply[5] = 0;
            send(reply);
            boolean pageEmpty = true;
            for (Rec x : held.values()) if (x.page == page) { pageEmpty = false; break; }
            if (pageEmpty && slot == 7) send(new byte[]{5, (byte) (page >> 8), (byte) page, 1});
        }
    }

    // ---- the radio between them ------------------------------------------------------
    static final class Radio implements BridgeLink.Radio {
        final Clock clock; final Gateway gw; final Random rnd;
        BridgeLink link;
        boolean adapter = true, inRange = true, connected, enabled, pendingAuto, pendingDirect;
        int gattToken;          // a closed GATT object never calls back
        boolean writing;
        boolean staleServices;  // Android's cached table without the ORUN service
        boolean calm;           // final phase: no lost frames or answers
        int lossEvery = 400;    // one indication in N is lost (reassembly drops its message)
        int writes, writeFails, lostAnswers, lostIndications, busy;
        Radio(Clock c, Gateway g, Random r) { clock = c; gw = g; rnd = r; }

        public boolean adapterOn() { return adapter; }

        public boolean connect(boolean auto) {
            if (!adapter) return false;
            if (rnd.nextInt(50) == 0) return false;                 // connectGatt returned null
            final int t = ++gattToken;
            if (auto) { pendingAuto = true; tryAuto(t); }
            else {
                pendingDirect = true;
                if (inRange && rnd.nextInt(6) != 0)
                    clock.after(200 + rnd.nextInt(800), () -> { if (t == gattToken && pendingDirect && inRange && adapter) up(); });
                else if (rnd.nextBoolean())                           // Android's status 133 after a while
                    clock.after(5_000 + rnd.nextInt(25_000), () -> { if (t == gattToken && pendingDirect) { pendingDirect = false; link.onDisconnected(133); } });
            }
            return true;
        }

        void tryAuto(int t) {
            clock.after(1_000 + rnd.nextInt(3_000), () -> {
                if (t != gattToken || !pendingAuto) return;
                if (inRange && adapter) up(); else tryAuto(t);
            });
        }

        void up() { pendingAuto = false; pendingDirect = false; connected = true; link.onConnected(); }

        public void discover() {
            final int t = gattToken;
            clock.after(100 + rnd.nextInt(400), () -> { if (t == gattToken && connected) link.onServices(!staleServices); });
        }

        public void refreshAndDiscover() { staleServices = false; discover(); }

        public boolean enableIndications() {
            final int t = gattToken;
            clock.after(50 + rnd.nextInt(100), () -> {
                if (t != gattToken || !connected) return;
                enabled = true; gw.begin(); link.onEnabled(true); indicate(t);
            });
            return true;
        }

        void indicate(int t) {          // the Gateway sends one indication at a time
            clock.after(8 + rnd.nextInt(30), () -> {
                if (t != gattToken || !connected) return;
                byte[] f = gw.out.pollFirst();
                if (f != null) {
                    if (!calm && rnd.nextInt(lossEvery) == 0) lostIndications++;        // a frame lost: message dropped by reassembly
                    else link.onFrame(f);
                }
                indicate(t);
            });
        }

        public boolean write(byte[] frame) {
            if (!connected || writing) return false;
            if (rnd.nextInt(30) == 0) { busy++; return false; }
            writing = true; writes++;
            final int t = gattToken;
            final boolean fail = rnd.nextInt(80) == 0, lose = !calm && rnd.nextInt(600) == 0;
            clock.after(10 + rnd.nextInt(40), () -> {
                if (t != gattToken || !connected) return;
                writing = false;
                if (lose) { lostAnswers++; return; }
                if (fail) { writeFails++; link.onWritten(false); return; }
                try { gw.frameIn(frame, clock.now); } catch (AssertionError e) { throw e; } catch (Exception e) { throw new RuntimeException(e); }
                link.onWritten(true);
            });
            return true;
        }

        public void priority(boolean high) { }

        public void close() {
            gattToken++;
            if (connected) gw.end();
            connected = false; enabled = false; writing = false; pendingAuto = false; pendingDirect = false;
        }

        /** Link loss seen by Android: the callback comes, the app then closes. */
        void drop() {
            if (!connected) return;
            gw.end(); connected = false; enabled = false; writing = false;
            link.onDisconnected(8);
        }
    }

    static ProductFrame.Keys KEYS;

    public static void main(String[] args) throws Exception {
        long epoch = 0; byte[] traffic = null, edge = null; long gwId = 0;
        Map<String, byte[]> objects = new LinkedHashMap<>();
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            String[] f = line.split("\\|", -1);
            if (f[0].equals("key")) {
                epoch = Long.parseLong(f[1]); traffic = BridgeWireCheck.hex(f[2]); edge = BridgeWireCheck.hex(f[3]);
                gwId = Long.parseUnsignedLong(f[4], 16);
            }
        }
        final long ep = epoch; final byte[] tk = traffic;
        KEYS = (dev, e) -> e == ep ? tk : null;
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            String[] f = line.split("\\|", -1);
            if (!f[0].equals("frame")) continue;
            byte[] obj = BridgeWireCheck.hex(f[1]);
            ProductFrame.Opened it = ProductFrame.open(obj, KEYS);
            if (it == null) throw new AssertionError("pool frame does not open");
            String id = BridgeCore.hexOf(it.device) + ":" + BridgeCore.hexOf(it.incarnation) + ":" + it.sequence;
            objects.putIfAbsent(id, obj);
        }
        if (objects.size() < 20) throw new AssertionError("too few records in vectors: " + objects.size());
        Path dir = Files.createTempDirectory("bridge-link");
        int seeds = args.length > 1 ? Integer.parseInt(args[1]) : 12, passed = 0;
        for (int seed = 1; seed <= seeds; seed++) {
            if (runSeed(seed, dir, gwId, edge, objects)) passed++;
        }
        System.out.println("BRIDGE-LINK " + passed + "/" + seeds + (passed == seeds ? " PASS" : " FAIL") +
                " (" + objects.size() + " records each)");
        if (passed != seeds) System.exit(1);
    }

    static boolean runSeed(int seed, Path dir, long gwId, byte[] edgeKey, Map<String, byte[]> objects) throws Exception {
        Random rnd = new Random(seed);
        Clock clock = new Clock();
        PySql sql = new PySql(dir.resolve("link" + seed + ".db").toString());
        try {
            BridgeStore store = new BridgeStore(sql);
            Gateway gw = new Gateway(gwId, edgeKey, rnd);
            gw.store = store;
            Radio radio = new Radio(clock, gw, rnd);
            radio.staleServices = seed % 3 == 0;
            BridgeCore core = new BridgeCore(store, KEYS, g -> g.equals(BridgeCore.gatewayHex(gwId)) ? edgeKey : null,
                    () -> clock.now / 1000);
            final int[] readyPeriods = {0}, stored = {0}, lora = {0};
            final List<String> states = new ArrayList<>();
            BridgeLink link = new BridgeLink(radio, clock, core, new BridgeLink.Sessions() {
                public void alive(long s) throws Exception { store.sessionAlive(s, "ble"); }
                public void end(long s) throws Exception { store.sessionEnd(s); }
            }, new BridgeLink.Events() {
                public void state(String s, String d) { states.add(s); if (s.equals(BridgeLink.READY)) readyPeriods[0]++; }
                public void changed() { }
                public void stored(int n) { stored[0] += n; }
                public void hello(String g) { }
                public void loraLine(String l) { if (l.startsWith("LORA QUEUED")) lora[0]++; }
                public void log(String l) { }
            });
            radio.link = link;
            List<Rec> pool = new ArrayList<>();
            for (Map.Entry<String, byte[]> e : objects.entrySet()) { Rec r = new Rec(); r.id = e.getKey(); r.object = e.getValue(); pool.add(r); }
            // Some records are already on the Gateway before the phone comes.
            int preload = pool.size() / 3;
            for (int i = 0; i < preload; i++) gw.receive(pool.get(i));
            link.start();
            long end = clock.now + 6 * 3_600_000L;
            int next = preload, lines = 0, linesOk = 0;
            final int[] lineDone = {0};
            while (clock.now < end) {
                clock.runUntil(clock.now + 1_000 + rnd.nextInt(20_000));
                int ev = rnd.nextInt(100);
                if (ev < 25 && next < pool.size() - 40) gw.receive(pool.get(next++));
                else if (ev < 30) radio.drop();
                else if (ev < 32) { radio.adapter = false; link.onAdapter(false); clock.runUntil(clock.now + rnd.nextInt(120_000)); radio.adapter = true; link.onAdapter(true); }
                else if (ev < 36) { radio.inRange = false; radio.drop(); clock.runUntil(clock.now + rnd.nextInt(ev == 33 ? 3_600_000 : 300_000)); radio.inRange = true; }
                else if (ev < 38) link.redump();
                else if (ev < 41 && link.ready()) { lines++; link.sendLine("APP LORA QUEUE 00", ok -> { lineDone[0]++; }); }
                else if (ev == 41) { link.stop(); clock.runUntil(clock.now + rnd.nextInt(60_000)); link.start(); }
            }
            // Then two quiet hours in range without drops. In the first one
            // the last records arrive and many frames are lost on the way: only
            // asking the Gateway again (DUMP) can find those records.
            radio.inRange = true; radio.adapter = true; link.onAdapter(true);
            radio.lossEvery = 25;
            long lossyEnd = clock.now + 3_600_000L;
            while (clock.now < lossyEnd) {
                clock.runUntil(clock.now + 30_000 + rnd.nextInt(60_000));
                if (next < pool.size()) gw.receive(pool.get(next++));
            }
            while (next < pool.size()) gw.receive(pool.get(next++));
            radio.calm = true;
            clock.runUntil(clock.now + 70 * 60_000L);
            List<String> problems = new ArrayList<>();
            if (!link.ready()) problems.add("not connected at the end: " + link.state() + " " + link.detail());
            if (!gw.held.isEmpty()) problems.add(gw.held.size() + " records still on the Gateway");
            BridgeStore.Counts c = store.counts();
            if (c.records != pool.size()) problems.add("phone holds " + c.records + " of " + pool.size());
            if (new HashSet<>(gw.handedOff).size() != gw.handedOff.size()) problems.add("a record was handed off twice");
            if (gw.rejected != 0) problems.add(gw.rejected + " ACCEPTs rejected (bad MAC/generation)");
            if (lineDone[0] != lines) problems.add("LoRa lines answered " + lineDone[0] + "/" + lines);
            int sessions = Integer.parseInt(sql.query("SELECT count(*) FROM sessions").get(0)[0]);
            if (sessions != readyPeriods[0]) problems.add("sessions " + sessions + " != connected periods " + readyPeriods[0]);
            List<BridgeStore.Change> ch = store.changes(0, 200);
            if (ch.size() != Math.min(200, pool.size())) problems.add("changes " + ch.size());
            for (BridgeStore.Change x : ch) if (x.firstSeen == null || x.rssi == null || !BridgeCore.gatewayHex(gwId).equals(x.gateway)) { problems.add("receipt missing"); break; }
            String tag = "seed " + seed + ": connected " + readyPeriods[0] + "x, writes " + radio.writes + " (busy " + radio.busy +
                    ", failed " + radio.writeFails + ", answer lost " + radio.lostAnswers + "), indications lost " + radio.lostIndications +
                    ", BUSY " + gw.busyReplies + ", HELLO lost " + gw.helloLost + ", LoRa " + lineDone[0] + "/" + lines;
            if (!problems.isEmpty()) { System.out.println("FAIL " + tag + "\n  " + String.join("\n  ", problems)); return false; }
            if (seed <= 3) System.out.println("ok   " + tag);
            return true;
        } finally {
            sql.close();
        }
    }
}
