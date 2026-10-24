// Capacitor SQLite (Android) implementation of the Edge Db.
// One connection, explicit transactions; every plugin call passes
// transaction=false so the plugin never wraps a statement on its own.
import { CapacitorSQLite, SQLiteConnection, type SQLiteDBConnection } from "@capacitor-community/sqlite";
import type { Db, Row, SqlValue } from "../core/db.ts";

export class CapacitorDb implements Db {
  private conn: SQLiteDBConnection;
  /** What SQLite reports after opening (shown in the app's status). */
  journal = "?";
  synchronous = "?";

  private constructor(conn: SQLiteDBConnection) { this.conn = conn; }

  static async open(name = "orun_edge"): Promise<CapacitorDb> {
    const sqlite = new SQLiteConnection(CapacitorSQLite);
    const existing = (await sqlite.isConnection(name, false)).result;
    const conn = existing
      ? await sqlite.retrieveConnection(name, false)
      : await sqlite.createConnection(name, false, "no-encryption", 1, false);
    await conn.open();
    const db = new CapacitorDb(conn);
    // Durability: COMMIT returns only after the data reached storage.
    try { await conn.query("PRAGMA journal_mode=WAL"); } catch { /* keep default */ }
    try { await conn.execute("PRAGMA synchronous=FULL", false); } catch { /* reported below */ }
    try {
      db.journal = String(Object.values((await conn.query("PRAGMA journal_mode")).values?.[0] ?? {})[0] ?? "?");
      db.synchronous = String(Object.values((await conn.query("PRAGMA synchronous")).values?.[0] ?? {})[0] ?? "?");
    } catch { /* diagnostics only */ }
    return db;
  }

  async execute(sql: string): Promise<void> { await this.conn.execute(sql, false); }

  async run(sql: string, params: SqlValue[] = []): Promise<void> {
    await this.conn.run(sql, params, false);
  }

  async query(sql: string, params: SqlValue[] = []): Promise<Row[]> {
    return ((await this.conn.query(sql, params)).values ?? []) as Row[];
  }

  async transaction<T>(fn: () => Promise<T>): Promise<T> {
    await this.conn.beginTransaction();
    try {
      const out = await fn();
      await this.conn.commitTransaction();
      return out;
    } catch (e) {
      try { await this.conn.rollbackTransaction(); } catch { /* already closed */ }
      throw e;
    }
  }
}
