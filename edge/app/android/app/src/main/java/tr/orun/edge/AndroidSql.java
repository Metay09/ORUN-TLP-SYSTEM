package tr.orun.edge;

import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Sql on Android's own SQLite (android.database.sqlite), for the native
 * bridge's file. Durability like the app's database: WAL and
 * synchronous=FULL, so COMMIT returns only after the data reached storage.
 */
final class AndroidSql implements Sql {
    private final SQLiteDatabase db;
    final String journal, synchronous;

    AndroidSql(File file) {
        db = SQLiteDatabase.openOrCreateDatabase(file, null);
        db.enableWriteAheadLogging();
        db.execSQL("PRAGMA synchronous=FULL");
        journal = pragma("PRAGMA journal_mode");
        synchronous = pragma("PRAGMA synchronous");
    }

    private String pragma(String sql) {
        try (Cursor c = db.rawQuery(sql, null)) {
            return c.moveToFirst() ? c.getString(0) : "?";
        } catch (RuntimeException e) {
            return "?";
        }
    }

    @Override
    public void exec(String sql, Object... args) {
        if (args == null || args.length == 0) db.execSQL(sql);
        else db.execSQL(sql, args);
    }

    @Override
    public List<String[]> query(String sql, Object... args) {
        String[] bind = null;
        if (args != null && args.length > 0) {
            bind = new String[args.length];
            for (int i = 0; i < args.length; i++) {
                if (args[i] == null) throw new IllegalArgumentException("null query argument");
                bind[i] = String.valueOf(args[i]);
            }
        }
        List<String[]> out = new ArrayList<>();
        try (Cursor c = db.rawQuery(sql, bind)) {
            int n = c.getColumnCount();
            while (c.moveToNext()) {
                String[] row = new String[n];
                for (int i = 0; i < n; i++) row[i] = c.isNull(i) ? null : c.getString(i);
                out.add(row);
            }
        }
        return out;
    }

    @Override public void begin() { db.beginTransaction(); }

    @Override
    public void commit() {
        db.setTransactionSuccessful();
        db.endTransaction();
    }

    @Override
    public void rollback() {
        try {
            if (db.inTransaction()) db.endTransaction();
        } catch (RuntimeException ignored) {
            // Nothing open any more.
        }
    }

    void close() {
        try { db.close(); } catch (RuntimeException ignored) { }
    }
}
