package tr.orun.edge;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * PC check only: the native bridge's Sql on real SQLite, through Python's
 * sqlite3 module in a child process. Query parameters are bound as text, the
 * way Android's rawQuery binds them; exec parameters keep their type; values
 * come back as text, like Cursor.getString(). Transactions are BEGIN
 * EXCLUSIVE, like SQLiteDatabase.beginTransaction().
 */
final class PySql implements Sql {
    private static final String SERVER = String.join("\n",
        "import sqlite3, sys",
        "db = sqlite3.connect(sys.argv[1], isolation_level=None)",
        "db.execute('PRAGMA synchronous=FULL')",
        "def args(t):",
        "    out = []",
        "    for a in (t.split(',') if t else []):",
        "        if a == 'n': out.append(None)",
        "        elif a[0] == 'i': out.append(int(a[1:]))",
        "        else: out.append(bytes.fromhex(a[1:]).decode())",
        "    return out",
        "def val(v):",
        "    if v is None: return 'n'",
        "    return 's' + str(v).encode().hex()",
        "for line in sys.stdin:",
        "    kind, _, rest = line.rstrip('\\n').partition('|')",
        "    sql, _, a = rest.partition('|')",
        "    try:",
        "        if kind == 'B': db.execute('BEGIN EXCLUSIVE'); print('OK')",
        "        elif kind == 'C': db.execute('COMMIT'); print('OK')",
        "        elif kind == 'R':",
        "            if db.in_transaction: db.execute('ROLLBACK')",
        "            print('OK')",
        "        elif kind == 'E': db.execute(bytes.fromhex(sql).decode(), args(a)); print('OK')",
        "        else:",
        "            rows = db.execute(bytes.fromhex(sql).decode(), args(a)).fetchall()",
        "            print('ROWS %d' % len(rows))",
        "            for r in rows: print(','.join(val(v) for v in r))",
        "    except Exception as e:",
        "        print('ERR ' + str(e).replace('\\n', ' '))",
        "    sys.stdout.flush()");

    private final Process proc;
    private final Writer in;
    private final BufferedReader out;
    int statements;

    PySql(String path) throws Exception {
        proc = new ProcessBuilder("python3", "-I", "-c", SERVER, path).redirectErrorStream(true).start();
        in = new OutputStreamWriter(proc.getOutputStream(), StandardCharsets.UTF_8);
        out = new BufferedReader(new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8));
    }

    void close() {
        try { in.close(); } catch (Exception ignored) { }
        proc.destroy();
    }

    private static String hex(String s) {
        StringBuilder b = new StringBuilder();
        for (byte v : s.getBytes(StandardCharsets.UTF_8)) b.append(String.format("%02x", v & 0xff));
        return b.toString();
    }

    private static String arg(Object a, boolean asText) {
        if (a == null) return "n";
        if (!asText && (a instanceof Long || a instanceof Integer)) return "i" + a;
        if (a instanceof String || a instanceof Long || a instanceof Integer) return "s" + hex(String.valueOf(a));
        throw new IllegalArgumentException("unsupported SQL argument " + a.getClass());
    }

    private List<String> send(String kind, String sql, Object[] args, boolean asText) throws Exception {
        statements++;
        StringBuilder line = new StringBuilder(kind).append('|');
        if (sql != null) line.append(hex(sql));
        line.append('|');
        for (int i = 0; args != null && i < args.length; i++) line.append(i == 0 ? "" : ",").append(arg(args[i], asText));
        in.write(line.append('\n').toString());
        in.flush();
        String head = out.readLine();
        if (head == null) throw new IllegalStateException("sqlite process ended");
        if (head.startsWith("ERR ")) throw new IllegalStateException(head.substring(4));
        List<String> rows = new ArrayList<>();
        if (head.startsWith("ROWS ")) {
            int n = Integer.parseInt(head.substring(5));
            for (int i = 0; i < n; i++) rows.add(out.readLine());
        }
        return rows;
    }

    @Override public void exec(String sql, Object... args) throws Exception { send("E", sql, args, false); }

    @Override
    public List<String[]> query(String sql, Object... args) throws Exception {
        List<String[]> out = new ArrayList<>();
        for (String line : send("Q", sql, args, true)) {
            String[] cells = line.isEmpty() ? new String[]{"n"} : line.split(",", -1);
            String[] row = new String[cells.length];
            for (int i = 0; i < cells.length; i++) {
                if (cells[i].equals("n")) continue;
                String h = cells[i].substring(1);
                byte[] b = new byte[h.length() / 2];
                for (int j = 0; j < b.length; j++) b[j] = (byte) Integer.parseInt(h.substring(2 * j, 2 * j + 2), 16);
                row[i] = new String(b, StandardCharsets.UTF_8);
            }
            out.add(row);
        }
        return out;
    }

    @Override public void begin() throws Exception { send("B", null, null, false); }
    @Override public void commit() throws Exception { send("C", null, null, false); }
    @Override public void rollback() { try { send("R", null, null, false); } catch (Exception ignored) { } }
}
