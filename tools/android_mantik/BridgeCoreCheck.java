package tr.orun.edge;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * PC check: BridgeCore + BridgeSpool against scenarios played through the
 * app's real EdgeCore (bridge_core_vectors.txt). Every step must give the
 * same ACCEPT bytes, counters, held count, queue length, gateway and number
 * of stored records / conflicts.
 *   bash tools/orun_android_mantik_test.sh
 */
public final class BridgeCoreCheck {
    /** In-memory BridgeSpool with the same primitives as the SQLite one. */
    static final class MemorySpool extends BridgeSpool {
        static final class Row { int family; byte[] plaintext, frame; }
        final Map<String, Row> rows = new HashMap<>();
        final List<String[]> conflicts = new ArrayList<>();
        private Map<String, Row> rowsBefore;
        private int conflictsBefore;
        static String key(byte[] d, byte[] i, long s) { return BridgeWireCheck.hex(d) + BridgeWireCheck.hex(i) + s; }
        protected void begin() { rowsBefore = new HashMap<>(rows); conflictsBefore = conflicts.size(); }
        protected void commit() { }
        protected void rollback() {
            rows.clear(); rows.putAll(rowsBefore);
            while (conflicts.size() > conflictsBefore) conflicts.remove(conflicts.size() - 1);
        }
        protected Saved find(byte[] d, byte[] i, long s) {
            Row r = rows.get(key(d, i, s));
            return r == null ? null : new Saved(r.family, r.plaintext);
        }
        protected boolean anyConflict(byte[] d, byte[] i, long s) {
            String k = key(d, i, s);
            for (String[] c : conflicts) if (c[0].equals(k)) return true;
            return false;
        }
        protected boolean sameConflict(byte[] d, byte[] i, long s, int f, byte[] p) {
            String k = key(d, i, s), pp = BridgeWireCheck.hex(p);
            for (String[] c : conflicts) if (c[0].equals(k) && c[1].equals(String.valueOf(f)) && c[2].equals(pp)) return true;
            return false;
        }
        protected int conflictCount() { return conflicts.size(); }
        protected void insert(Arrival a) {
            Row r = new Row(); r.family = a.item.family; r.plaintext = a.item.plaintext; r.frame = a.item.frame;
            rows.put(key(a.item.device, a.item.incarnation, a.item.sequence), r);
        }
        protected void insertConflict(Arrival a) {
            conflicts.add(new String[]{key(a.item.device, a.item.incarnation, a.item.sequence),
                    String.valueOf(a.item.family), BridgeWireCheck.hex(a.item.plaintext), BridgeWireCheck.hex(a.item.frame)});
        }
        protected void receipt(Arrival a) { }
        protected byte[] storedFrame(Arrival a, boolean conflict) {
            String k = key(a.item.device, a.item.incarnation, a.item.sequence);
            if (!conflict) { Row r = rows.get(k); return r == null ? null : r.frame; }
            String pp = BridgeWireCheck.hex(a.item.plaintext);
            for (String[] c : conflicts)
                if (c[0].equals(k) && c[1].equals(String.valueOf(a.item.family)) && c[2].equals(pp))
                    return BridgeWireCheck.hex(c[3]);
            return null;
        }
    }

    interface Store { BridgeSpool spool(); long records() throws Exception; long conflicts() throws Exception; void close(); }

    public static void main(String[] args) throws Exception {
        int a = run(args[0], () -> {
            MemorySpool m = new MemorySpool();
            return new Store() {
                public BridgeSpool spool() { return m; }
                public long records() { return m.rows.size(); }
                public long conflicts() { return m.conflicts.size(); }
                public void close() { }
            };
        }, "BRIDGE-CORE");
        // The same scenarios on the phone's real store: SQLite, Android-style binding.
        java.nio.file.Path dir = Files.createTempDirectory("bridge-store");
        final int[] n = {0};
        int b = run(args[0], () -> {
            PySql sql = new PySql(dir.resolve("s" + (n[0]++) + ".db").toString());
            BridgeStore st = new BridgeStore(sql);
            return new Store() {
                public BridgeSpool spool() { return st; }
                public long records() throws Exception { return st.counts().records; }
                public long conflicts() throws Exception { return st.counts().conflictsKept; }
                public void close() { sql.close(); }
            };
        }, "BRIDGE-STORE");
        if (a + b != 0) System.exit(1);
    }

    interface Factory { Store make() throws Exception; }

    static int run(String path, Factory factory, String label) throws Exception {
        long epoch = 0;
        byte[] traffic = null, edge = null;
        BridgeCore core = null;
        Store spool = null;
        final long[] now = {0};
        int steps = 0, failed = 0, scenario = -1;
        boolean skipping = false;
        for (String line : Files.readAllLines(Paths.get(path))) {
            if (line.startsWith("#") || line.isEmpty()) continue;
            String[] f = line.split("\\|", -1);
            if (f[0].equals("key")) {
                epoch = Long.parseLong(f[1]); traffic = BridgeWireCheck.hex(f[2]); edge = BridgeWireCheck.hex(f[3]);
                continue;
            }
            if (f[0].equals("scenario")) {
                scenario = Integer.parseInt(f[1]);
                skipping = false;
                final boolean withKey = f[2].equals("1");
                final long ep = epoch; final byte[] tk = traffic, ek = edge;
                if (spool != null) spool.close();
                spool = factory.make();
                now[0] = 1_800_000_000L;
                core = new BridgeCore(spool.spool(), (dev, e) -> e == ep ? tk : null, gw -> withKey ? ek : null, () -> now[0]);
                continue;
            }
            // step|op|accept|counts|handed|rejected|held|pending|gw|records|conflicts
            if (skipping) continue;
            String op = f[1];
            if (op.startsWith("msg:")) core.handleMessage(BridgeWireCheck.hex(op.substring(4)));
            else if (op.startsWith("tick:")) now[0] += Long.parseLong(op.substring(5));
            else core.sessionEnd();
            byte[] accept = core.nextAcceptMessage(now[0] * 1000);
            Integer held = core.heldNow();
            String got = String.join("|", accept == null ? "-" : BridgeWireCheck.hex(accept),
                    core.counts[0] + "," + core.counts[1] + "," + core.counts[2] + "," + core.counts[3] + "," + core.counts[4],
                    String.valueOf(core.handedOff), String.valueOf(core.acceptRejected),
                    held == null ? "null" : String.valueOf(held), String.valueOf(core.pendingAccepts()),
                    core.gatewayId == null ? "null" : BridgeCore.gatewayHex(core.gatewayId),
                    String.valueOf(spool.records()), String.valueOf(spool.conflicts()));
            String want = String.join("|", Arrays.copyOfRange(f, 2, f.length));
            steps++;
            if (!want.equals(got)) {
                failed++;
                if (failed <= 5) System.out.println("FAIL scenario " + scenario + " step " + op.substring(0, Math.min(40, op.length())) +
                        "\n  want " + want.substring(Math.max(0, want.length() - 120)) + "\n  got  " + got.substring(Math.max(0, got.length() - 120)));
                // One divergence spoils the rest of the scenario: skip to the next.
                skipping = true;
            }
        }
        if (spool != null) spool.close();
        System.out.println(label + " " + (steps - failed) + "/" + steps + (failed == 0 ? " PASS" : " FAIL"));
        return failed;
    }
}
