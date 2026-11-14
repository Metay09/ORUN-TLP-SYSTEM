package tr.orun.edge;

import java.util.List;

/**
 * The few SQL calls the native bridge's store needs. On the phone:
 * android.database.sqlite (AndroidSql); in the PC check: the same statements
 * on real SQLite through Python's sqlite3 (tools/android_mantik/PySql.java).
 *
 * Query parameters are always bound as text, the way Android's rawQuery
 * binds them, so every comparison must rely on the column's affinity.
 * exec parameters keep their type (Long/Integer/String/null).
 */
interface Sql {
    void exec(String sql, Object... args) throws Exception;

    /** Every value as text (SQLite's own conversion); SQL NULL as null. */
    List<String[]> query(String sql, Object... args) throws Exception;

    void begin() throws Exception;

    void commit() throws Exception;

    /** Safe to call when no transaction is open. */
    void rollback();
}
