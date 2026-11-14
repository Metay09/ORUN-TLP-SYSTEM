package tr.orun.edge;

import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The native bridge's own phone ledger (v1 step 7): an SQLite file next to
 * the app's database, never the app's file itself (two SQLite libraries in
 * one process must not share a database file). Same tables and values as the
 * app's core/db.ts so the app can take the rows over unchanged:
 * binary as upper-case hex TEXT, first exact frame wins, conflicts kept.
 *
 * Every row carries a change number (rev). The app asks "what changed after
 * rev N" (changes()), and an upload result moves the row's rev again so the
 * app also learns that the server has it.
 *
 * Not thread safe: the service runs every call on its one worker thread.
 */
final class BridgeStore extends BridgeSpool {
    static final String[] SCHEMA = {
        "CREATE TABLE IF NOT EXISTS observations (" +
            " device_id TEXT NOT NULL CHECK(length(device_id)=16)," +
            " incarnation TEXT NOT NULL CHECK(length(incarnation)=16)," +
            " sequence INTEGER NOT NULL CHECK(sequence>0)," +
            " family INTEGER NOT NULL CHECK(family IN (1,2))," +
            " plaintext TEXT NOT NULL CHECK(length(plaintext) IN (96,128))," +
            " frame TEXT NOT NULL CHECK(length(frame) IN (168,200))," +
            " rev INTEGER NOT NULL," +
            " PRIMARY KEY (device_id,incarnation,sequence))",
        "CREATE INDEX IF NOT EXISTS observations_rev ON observations(rev)",
        "CREATE TABLE IF NOT EXISTS conflicts (" +
            " device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL," +
            " family INTEGER NOT NULL, plaintext TEXT NOT NULL, frame TEXT NOT NULL, rev INTEGER NOT NULL," +
            " PRIMARY KEY (device_id,incarnation,sequence,family,plaintext))",
        "CREATE INDEX IF NOT EXISTS conflicts_rev ON conflicts(rev)",
        "CREATE TABLE IF NOT EXISTS receipts (" +
            " device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL," +
            " first_seen_utc INTEGER NOT NULL, gw_rssi INTEGER, gw_snr INTEGER, gateway TEXT," +
            " PRIMARY KEY (device_id,incarnation,sequence))",
        "CREATE TABLE IF NOT EXISTS sessions (" +
            " id INTEGER PRIMARY KEY AUTOINCREMENT, started_utc INTEGER NOT NULL," +
            " alive_utc INTEGER NOT NULL, transport TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS uploads (" +
            " device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL," +
            " state TEXT NOT NULL CHECK(state IN ('STORED','DUPLICATE','CONFLICT','REJECTED'))," +
            " utc INTEGER NOT NULL," +
            " PRIMARY KEY (device_id,incarnation,sequence))",
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
    };
    static final int BATCH = 100, SESSIONS_MAX = 64, CHANGES_MAX = 200;
    /** Uploaded rows the app already took over are dropped after this long. */
    static final long KEEP_S = 30L * 86_400;

    private final Sql sql;
    private Long session;

    BridgeStore(Sql sql) throws Exception {
        this.sql = sql;
        for (String s : SCHEMA) sql.exec(s);
        sql.exec("INSERT OR IGNORE INTO meta (key,value) VALUES ('rev','0')");
        if (meta("db_id") == null) setMeta("db_id", randomHex(8));
        if (meta("edge_id") == null) setMeta("edge_id", "phone-" + randomHex(6).toLowerCase(Locale.ROOT));
    }

    private static String randomHex(int bytes) {
        byte[] b = new byte[bytes];
        new SecureRandom().nextBytes(b);
        return BridgeCore.hexOf(b);
    }

    String meta(String key) throws Exception {
        List<String[]> r = sql.query("SELECT value FROM meta WHERE key=?", key);
        return r.isEmpty() ? null : r.get(0)[0];
    }

    void setMeta(String key, String value) throws Exception {
        sql.exec("INSERT OR REPLACE INTO meta (key,value) VALUES (?,?)", key, value);
    }

    /** This database's identity: a new file means the app starts its copy over. */
    String dbId() throws Exception { return meta("db_id"); }

    /** This bridge's id at the server (a separate Edge from the app's own USB path). */
    String edgeId() throws Exception { return meta("edge_id"); }

    private long nextRev() throws Exception {
        sql.exec("UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='rev'");
        return Long.parseLong(meta("rev"));
    }

    // ---- BridgeSpool primitives ---------------------------------------------
    @Override protected void begin() throws Exception { sql.begin(); }
    @Override protected void commit() throws Exception { sql.commit(); }
    @Override protected void rollback() { sql.rollback(); }

    @Override
    protected Saved find(byte[] device, byte[] incarnation, long sequence) throws Exception {
        List<String[]> r = sql.query(
            "SELECT family,plaintext FROM observations WHERE device_id=? AND incarnation=? AND sequence=?",
            BridgeCore.hexOf(device), BridgeCore.hexOf(incarnation), sequence);
        return r.isEmpty() ? null : new Saved(Integer.parseInt(r.get(0)[0]), unhex(r.get(0)[1]));
    }

    @Override
    protected boolean anyConflict(byte[] device, byte[] incarnation, long sequence) throws Exception {
        return !sql.query("SELECT 1 FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? LIMIT 1",
            BridgeCore.hexOf(device), BridgeCore.hexOf(incarnation), sequence).isEmpty();
    }

    @Override
    protected boolean sameConflict(byte[] device, byte[] incarnation, long sequence, int family,
                                   byte[] plaintext) throws Exception {
        return !sql.query("SELECT 1 FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? " +
                "AND family=? AND plaintext=?", BridgeCore.hexOf(device), BridgeCore.hexOf(incarnation),
            sequence, family, BridgeCore.hexOf(plaintext)).isEmpty();
    }

    @Override
    protected int conflictCount() throws Exception {
        return Integer.parseInt(sql.query("SELECT count(*) FROM conflicts").get(0)[0]);
    }

    @Override
    protected void insert(Arrival a) throws Exception {
        ProductFrame.Opened it = a.item;
        sql.exec("INSERT INTO observations (device_id,incarnation,sequence,family,plaintext,frame,rev) " +
                "VALUES (?,?,?,?,?,?,?)", BridgeCore.hexOf(it.device), BridgeCore.hexOf(it.incarnation),
            it.sequence, it.family, BridgeCore.hexOf(it.plaintext), BridgeCore.hexOf(it.frame), nextRev());
    }

    @Override
    protected void insertConflict(Arrival a) throws Exception {
        ProductFrame.Opened it = a.item;
        sql.exec("INSERT INTO conflicts (device_id,incarnation,sequence,family,plaintext,frame,rev) " +
                "VALUES (?,?,?,?,?,?,?)", BridgeCore.hexOf(it.device), BridgeCore.hexOf(it.incarnation),
            it.sequence, it.family, BridgeCore.hexOf(it.plaintext), BridgeCore.hexOf(it.frame), nextRev());
    }

    @Override
    protected void receipt(Arrival a) throws Exception {
        ProductFrame.Opened it = a.item;
        String d = BridgeCore.hexOf(it.device), i = BridgeCore.hexOf(it.incarnation);
        // First sighting wins (core/edge.ts receipt()): a later dump is not a new arrival.
        sql.exec("INSERT OR IGNORE INTO receipts (device_id,incarnation,sequence,first_seen_utc,gw_rssi,gw_snr," +
                "gateway) VALUES (?,?,?,?,?,?,?)", d, i, it.sequence, a.nowSec, a.rssi, a.snr,
            BridgeCore.gatewayHex(a.gateway));
        if (a.rssi != null)
            sql.exec("UPDATE receipts SET gw_rssi=?, gw_snr=? WHERE device_id=? AND incarnation=? AND sequence=? " +
                "AND gw_rssi IS NULL", a.rssi, a.snr, d, i, it.sequence);
    }

    @Override
    protected byte[] storedFrame(Arrival a, boolean conflict) throws Exception {
        ProductFrame.Opened it = a.item;
        List<String[]> r = conflict
            ? sql.query("SELECT frame FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? " +
                    "AND family=? AND plaintext=?", BridgeCore.hexOf(it.device), BridgeCore.hexOf(it.incarnation),
                it.sequence, it.family, BridgeCore.hexOf(it.plaintext))
            : sql.query("SELECT frame FROM observations WHERE device_id=? AND incarnation=? AND sequence=?",
                BridgeCore.hexOf(it.device), BridgeCore.hexOf(it.incarnation), it.sequence);
        return r.isEmpty() ? null : unhex(r.get(0)[0]);
    }

    // ---- sessions (coverage evidence, same as core/edge.ts) ------------------
    /** A Gateway is attached and being read right now. */
    void sessionAlive(long nowSec, String transport) throws Exception {
        if (session == null) {
            sql.exec("INSERT INTO sessions (started_utc,alive_utc,transport) VALUES (?,?,?)", nowSec, nowSec, transport);
            session = Long.parseLong(sql.query("SELECT max(id) FROM sessions").get(0)[0]);
        } else {
            sql.exec("UPDATE sessions SET alive_utc=? WHERE id=?", nowSec, session);
        }
    }

    void sessionEnd(long nowSec) throws Exception {
        if (session != null) sql.exec("UPDATE sessions SET alive_utc=? WHERE id=?", nowSec, session);
        session = null;
    }

    // ---- upload to the account server (core/sync.ts Uploader) ---------------
    static final class Outgoing {
        long rowid;
        String device, incarnation, frame, gateway;
        long sequence;
        Long firstSeen;
        Integer rssi, snr;
    }

    static final class Session {
        long id, started, alive;
        String transport;
    }

    static final class Batch {
        final List<Outgoing> rows = new ArrayList<>();
        final List<Session> sessions = new ArrayList<>();
        long sessionsFrom;
        String sessionsKey;
        boolean sessionsChanged;
        Long nextRowid;
    }

    /** Newest pending records first; beforeRowid continues one drain pass. */
    Batch nextBatch(Long beforeRowid) throws Exception {
        Batch b = new Batch();
        String q = "SELECT o.rowid,o.device_id,o.incarnation,o.sequence,o.frame,r.first_seen_utc,r.gw_rssi,r.gw_snr," +
            "r.gateway FROM observations o LEFT JOIN receipts r ON r.device_id=o.device_id AND " +
            "r.incarnation=o.incarnation AND r.sequence=o.sequence LEFT JOIN uploads u ON " +
            "u.device_id=o.device_id AND u.incarnation=o.incarnation AND u.sequence=o.sequence " +
            "WHERE u.state IS NULL " + (beforeRowid == null ? "" : "AND o.rowid < ? ") +
            "ORDER BY o.rowid DESC LIMIT " + BATCH;
        List<String[]> rows = beforeRowid == null ? sql.query(q) : sql.query(q, beforeRowid);
        for (String[] r : rows) {
            Outgoing o = new Outgoing();
            o.rowid = Long.parseLong(r[0]); o.device = r[1]; o.incarnation = r[2];
            o.sequence = Long.parseLong(r[3]); o.frame = r[4];
            o.firstSeen = r[5] == null ? null : Long.parseLong(r[5]);
            o.rssi = r[6] == null ? null : Integer.parseInt(r[6]);
            o.snr = r[7] == null ? null : Integer.parseInt(r[7]);
            o.gateway = r[8];
            b.rows.add(o);
        }
        // The last sent session keeps growing while attached: resend from it.
        String from = meta("sessions_from");
        b.sessionsFrom = from == null ? 1 : Long.parseLong(from);
        StringBuilder key = new StringBuilder();
        for (String[] s : sql.query("SELECT id,started_utc,alive_utc,transport FROM sessions WHERE id>=? " +
                "ORDER BY id LIMIT " + SESSIONS_MAX, b.sessionsFrom)) {
            Session x = new Session();
            x.id = Long.parseLong(s[0]); x.started = Long.parseLong(s[1]); x.alive = Long.parseLong(s[2]);
            x.transport = s[3];
            b.sessions.add(x);
            key.append(x.id).append(',').append(x.started).append(',').append(x.alive).append(',')
                .append(x.transport).append(';');
        }
        b.sessionsKey = key.toString();
        b.sessionsChanged = !b.sessions.isEmpty() && !b.sessionsKey.equals(meta("sessions_sent"));
        b.nextRowid = b.rows.isEmpty() ? null : b.rows.get(b.rows.size() - 1).rowid;
        return b;
    }

    static boolean isFinal(String state) {
        return "STORED".equals(state) || "DUPLICATE".equals(state) || "CONFLICT".equals(state) ||
            "REJECTED".equals(state);
    }

    /** The server's answer, one word per row in order. Returns how many are settled. */
    int settle(Batch b, List<String> results, boolean sessionsOk, long nowSec) throws Exception {
        if (results.size() != b.rows.size()) throw new IllegalArgumentException("results");
        int settled = 0;
        sql.begin();
        try {
            for (int i = 0; i < b.rows.size(); i++) {
                if (!isFinal(results.get(i))) continue;            // UNCERTAIN: send again later
                Outgoing o = b.rows.get(i);
                sql.exec("INSERT OR REPLACE INTO uploads (device_id,incarnation,sequence,state,utc) VALUES (?,?,?,?,?)",
                    o.device, o.incarnation, o.sequence, results.get(i), nowSec);
                sql.exec("UPDATE observations SET rev=? WHERE device_id=? AND incarnation=? AND sequence=?",
                    nextRev(), o.device, o.incarnation, o.sequence);
                settled++;
            }
            if (sessionsOk && !b.sessions.isEmpty()) {
                setMeta("sessions_from", String.valueOf(Math.max(b.sessionsFrom, b.sessions.get(b.sessions.size() - 1).id)));
                setMeta("sessions_sent", b.sessionsKey);
            }
            sql.commit();
        } catch (Exception e) {
            sql.rollback();
            throw e;
        }
        return settled;
    }

    /** Same numbers as the app's Uploader.counts(). */
    static final class Counts { long records, uploaded, pending, conflict, rejected, conflictsKept; }

    Counts counts() throws Exception {
        Counts c = new Counts();
        c.records = Long.parseLong(sql.query("SELECT count(*) FROM observations").get(0)[0]);
        long answered = 0;
        for (String[] r : sql.query("SELECT state, count(*) FROM uploads GROUP BY state")) {
            long n = Long.parseLong(r[1]);
            answered += n;
            if ("STORED".equals(r[0]) || "DUPLICATE".equals(r[0])) c.uploaded += n;
            else if ("CONFLICT".equals(r[0])) c.conflict += n;
            else if ("REJECTED".equals(r[0])) c.rejected += n;
        }
        c.pending = Math.max(0, c.records - answered);
        c.conflictsKept = Long.parseLong(sql.query("SELECT count(*) FROM conflicts").get(0)[0]);
        return c;
    }

    // ---- what the app takes over ---------------------------------------------
    static final class Change {
        long rev;
        boolean conflict;
        String device, incarnation, plaintext, frame, gateway, upload;
        long sequence;
        int family;
        Long firstSeen;
        Integer rssi, snr;
    }

    /** Rows whose rev is above `after`, oldest change first, at most `limit`. */
    List<Change> changes(long after, int limit) throws Exception {
        int n = Math.max(1, Math.min(limit, CHANGES_MAX));
        String cols = "x.rev,x.device_id,x.incarnation,x.sequence,x.family,x.plaintext,x.frame," +
            "r.first_seen_utc,r.gw_rssi,r.gw_snr,r.gateway";
        String join = " LEFT JOIN receipts r ON r.device_id=x.device_id AND r.incarnation=x.incarnation AND " +
            "r.sequence=x.sequence";
        List<String[]> obs = sql.query("SELECT " + cols + ",u.state FROM observations x" + join +
            " LEFT JOIN uploads u ON u.device_id=x.device_id AND u.incarnation=x.incarnation AND " +
            "u.sequence=x.sequence WHERE x.rev>? ORDER BY x.rev LIMIT " + n, after);
        List<String[]> con = sql.query("SELECT " + cols + ",NULL FROM conflicts x" + join +
            " WHERE x.rev>? ORDER BY x.rev LIMIT " + n, after);
        List<Change> out = new ArrayList<>();
        int a = 0, c = 0;
        while (out.size() < n && (a < obs.size() || c < con.size())) {
            boolean takeConflict = a >= obs.size() ||
                (c < con.size() && Long.parseLong(con.get(c)[0]) < Long.parseLong(obs.get(a)[0]));
            String[] r = takeConflict ? con.get(c++) : obs.get(a++);
            Change x = new Change();
            x.conflict = takeConflict;
            x.rev = Long.parseLong(r[0]); x.device = r[1]; x.incarnation = r[2]; x.sequence = Long.parseLong(r[3]);
            x.family = Integer.parseInt(r[4]); x.plaintext = r[5]; x.frame = r[6];
            x.firstSeen = r[7] == null ? null : Long.parseLong(r[7]);
            x.rssi = r[8] == null ? null : Integer.parseInt(r[8]);
            x.snr = r[9] == null ? null : Integer.parseInt(r[9]);
            x.gateway = r[10]; x.upload = r[11];
            out.add(x);
        }
        return out;
    }

    /** The app holds every change up to `rev` (only ever moves forward). */
    void takenUpTo(long rev) throws Exception {
        String have = meta("taken");
        if (have == null || Long.parseLong(have) < rev) setMeta("taken", String.valueOf(rev));
    }

    /**
     * Drops rows the server has settled, the app took over and that arrived
     * more than KEEP_S ago. Conflicts (evidence) and every identity that has
     * one are kept (a conflict must keep quarantining it). A dropped record
     * the Gateway sends again is simply stored and uploaded again
     * (the server answers DUPLICATE).
     */
    int prune(long nowSec) throws Exception {
        String taken = meta("taken");
        if (taken == null) return 0;
        String old = "SELECT o.device_id,o.incarnation,o.sequence FROM observations o JOIN uploads u ON " +
            "u.device_id=o.device_id AND u.incarnation=o.incarnation AND u.sequence=o.sequence JOIN receipts r ON " +
            "r.device_id=o.device_id AND r.incarnation=o.incarnation AND r.sequence=o.sequence " +
            "WHERE o.rev<=? AND r.first_seen_utc<? AND NOT EXISTS (SELECT 1 FROM conflicts c WHERE " +
            "c.device_id=o.device_id AND c.incarnation=o.incarnation AND c.sequence=o.sequence) LIMIT 500";
        List<String[]> rows = sql.query(old, taken, nowSec - KEEP_S);
        if (rows.isEmpty()) return 0;
        sql.begin();
        try {
            for (String[] r : rows)
                for (String t : new String[]{"observations", "receipts", "uploads"})
                    sql.exec("DELETE FROM " + t + " WHERE device_id=? AND incarnation=? AND sequence=?",
                        r[0], r[1], Long.parseLong(r[2]));
            sql.commit();
        } catch (Exception e) {
            sql.rollback();
            throw e;
        }
        return rows.size();
    }

    static byte[] unhex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }
}
