// Minimal async SQL surface the Edge core needs. Implemented by Capacitor
// SQLite on the phone (src/platform/sqliteDb.ts) and node:sqlite in tests.
// Binary values are stored as upper-case hex TEXT: identical on every
// SQLite binding, and easy to inspect.
export type SqlValue = string | number | null;
export type Row = Record<string, SqlValue>;

export interface Db {
  /** One statement without parameters (DDL, PRAGMA). */
  execute(sql: string): Promise<void>;
  run(sql: string, params?: SqlValue[]): Promise<void>;
  query(sql: string, params?: SqlValue[]): Promise<Row[]>;
  /**
   * Runs fn inside BEGIN IMMEDIATE ... COMMIT; ROLLBACK if fn throws.
   * Resolves only after COMMIT has returned (synchronous=FULL).
   */
  transaction<T>(fn: () => Promise<T>): Promise<T>;
}

export const SCHEMA: string[] = [
  // Verified observations: first exact frame wins, never overwritten.
  `CREATE TABLE IF NOT EXISTS observations (
     device_id TEXT NOT NULL CHECK(length(device_id)=16),
     incarnation TEXT NOT NULL CHECK(length(incarnation)=16),
     sequence INTEGER NOT NULL CHECK(sequence>0),
     family INTEGER NOT NULL CHECK(family IN (1,2)),
     plaintext TEXT NOT NULL CHECK(length(plaintext) IN (96,128)),
     frame TEXT NOT NULL CHECK(length(frame) IN (168,200)),
     PRIMARY KEY (device_id,incarnation,sequence))`,
  // Same logical identity, different authenticated content: kept as evidence.
  `CREATE TABLE IF NOT EXISTS conflicts (
     device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL,
     family INTEGER NOT NULL, plaintext TEXT NOT NULL, frame TEXT NOT NULL,
     PRIMARY KEY (device_id,incarnation,sequence,family,plaintext))`,
  // First arrival at this phone + the Gateway's RX signal of that record.
  `CREATE TABLE IF NOT EXISTS receipts (
     device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL,
     first_seen_utc INTEGER NOT NULL, gw_rssi INTEGER, gw_snr INTEGER, gateway TEXT,
     PRIMARY KEY (device_id,incarnation,sequence))`,
  // Periods this phone was attached to a live Gateway (coverage evidence).
  `CREATE TABLE IF NOT EXISTS sessions (
     id INTEGER PRIMARY KEY AUTOINCREMENT, started_utc INTEGER NOT NULL,
     alive_utc INTEGER NOT NULL, transport TEXT NOT NULL)`,
  // SF5L8: what the backend answered for each record (absent = not yet
  // uploaded). UNCERTAIN / network failure leave no row: sent again later.
  `CREATE TABLE IF NOT EXISTS uploads (
     device_id TEXT NOT NULL, incarnation TEXT NOT NULL, sequence INTEGER NOT NULL,
     state TEXT NOT NULL CHECK(state IN ('STORED','DUPLICATE','CONFLICT','REJECTED')),
     utc INTEGER NOT NULL,
     PRIMARY KEY (device_id,incarnation,sequence))`,
  `CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)`,
  // Step 3A / D1: read (D2A traffic) keys of the owner's Trackers, from the
  // backend. Never a root key.
  `CREATE TABLE IF NOT EXISTS read_keys (
     device_id TEXT NOT NULL CHECK(length(device_id)=16),
     key_epoch INTEGER NOT NULL,
     traffic_key TEXT NOT NULL CHECK(length(traffic_key)=32),
     PRIMARY KEY (device_id,key_epoch))`,
];
