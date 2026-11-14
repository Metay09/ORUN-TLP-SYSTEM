// v1 step 7: the phone's native Gateway bridge (Android GatewayBridgeService)
// keeps the Gateway connected, verifies and stores every record in its own
// SQLite file and uploads it itself, also with the app closed. When the app
// runs it takes those rows over into this ledger, so the map, herd and
// alerts work without Internet exactly as with the in-app link.
//
// The rows were authenticated by the bridge with the same read keys and the
// same ledger rules (BridgeSpool.java = ledger.ts); here they are only
// copied: first frame wins, a differing frame is kept as a conflict, the
// first arrival wins. The copies are marked "bridged" so this app's own
// Uploader never sends them a second time; the bridge's upload answer is
// copied too (it comes again, with a newer rev, once the server answered).
import type { Db } from "./db.ts";

export interface BridgeRow {
  rev: number;
  conflict: boolean;
  device: string; incarnation: string; sequence: number;
  family: number; plaintext: string; frame: string;
  firstSeen: number | null; rssi: number | null; snr: number | null; gateway: string | null;
  upload: string | null;
}

const FINAL = new Set(["STORED", "DUPLICATE", "CONFLICT", "REJECTED"]);
const HEX = (n: number[]) => (v: unknown) => typeof v === "string" && n.includes(v.length) && /^[0-9A-F]*$/.test(v);
const ID = HEX([16]), PLAIN = HEX([96, 128]), FRAME = HEX([168, 200]);
const int = (v: unknown, lo: number, hi: number) => typeof v === "number" && Number.isInteger(v) && v >= lo && v <= hi;
const optInt = (v: unknown, lo: number, hi: number) => v === null || int(v, lo, hi);

/** A row as the bridge stores it; anything else is skipped, never half-written. */
export function validRow(r: BridgeRow): boolean {
  return int(r.rev, 1, Number.MAX_SAFE_INTEGER) && typeof r.conflict === "boolean" &&
    ID(r.device) && ID(r.incarnation) && int(r.sequence, 1, 0xffffffff) &&
    (r.family === 1 || r.family === 2) && PLAIN(r.plaintext) && FRAME(r.frame) &&
    optInt(r.firstSeen, 1, 2 ** 32) && optInt(r.rssi, -200, 50) && optInt(r.snr, -128, 127) &&
    (r.gateway === null || ID(r.gateway)) && (r.upload === null || FINAL.has(r.upload));
}

export interface Cursor { db: string; rev: number; }

export async function readCursor(db: Db): Promise<Cursor> {
  const rows = await db.query("SELECT key,value FROM meta WHERE key IN ('bridge_db','bridge_rev')");
  const m = new Map(rows.map((r) => [String(r.key), String(r.value)]));
  return { db: m.get("bridge_db") ?? "", rev: Number(m.get("bridge_rev") ?? "0") || 0 };
}

/** Copies one page of bridge rows and moves the cursor, in one transaction.
 * Returns how many rows were new to this ledger (observations + conflicts). */
export async function importRows(db: Db, dbId: string, rows: BridgeRow[], last: number,
                                 nowSec = Math.floor(Date.now() / 1000)): Promise<number> {
  let added = 0;
  await db.transaction(async () => {
    for (const r of rows) {
      if (!validRow(r)) continue;
      const key = [r.device, r.incarnation, r.sequence];
      if (r.conflict) {
        const had = await db.query(
          "SELECT 1 AS x FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? AND family=? AND plaintext=?",
          [...key, r.family, r.plaintext]);
        if (!had.length) {
          await db.run("INSERT INTO conflicts (device_id,incarnation,sequence,family,plaintext,frame) VALUES (?,?,?,?,?,?)",
                       [...key, r.family, r.plaintext, r.frame]);
          added++;
        }
        continue;
      }
      const saved = await db.query(
        "SELECT family,plaintext FROM observations WHERE device_id=? AND incarnation=? AND sequence=?", key);
      if (!saved.length) {
        await db.run("INSERT INTO observations (device_id,incarnation,sequence,family,plaintext,frame) VALUES (?,?,?,?,?,?)",
                     [...key, r.family, r.plaintext, r.frame]);
        added++;
      } else if (Number(saved[0].family) !== r.family || saved[0].plaintext !== r.plaintext) {
        // Same identity, other content than this ledger already has: evidence.
        const had = await db.query(
          "SELECT 1 AS x FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? AND family=? AND plaintext=?",
          [...key, r.family, r.plaintext]);
        if (!had.length) {
          await db.run("INSERT INTO conflicts (device_id,incarnation,sequence,family,plaintext,frame) VALUES (?,?,?,?,?,?)",
                       [...key, r.family, r.plaintext, r.frame]);
          added++;
        }
      }
      if (r.firstSeen !== null) {
        await db.run(
          "INSERT OR IGNORE INTO receipts (device_id,incarnation,sequence,first_seen_utc,gw_rssi,gw_snr,gateway) VALUES (?,?,?,?,?,?,?)",
          [...key, r.firstSeen, r.rssi, r.snr, r.gateway]);
        if (r.rssi !== null)
          await db.run("UPDATE receipts SET gw_rssi=?, gw_snr=? WHERE device_id=? AND incarnation=? AND sequence=? AND gw_rssi IS NULL",
                       [r.rssi, r.snr, ...key]);
      }
      await db.run("INSERT OR IGNORE INTO bridged (device_id,incarnation,sequence) VALUES (?,?,?)", key);
      if (r.upload !== null)
        await db.run("INSERT OR IGNORE INTO uploads (device_id,incarnation,sequence,state,utc) VALUES (?,?,?,?,?)",
                     [...key, r.upload, nowSec]);
    }
    await db.run("INSERT OR REPLACE INTO meta (key,value) VALUES ('bridge_db',?)", [dbId]);
    await db.run("INSERT OR REPLACE INTO meta (key,value) VALUES ('bridge_rev',?)", [String(last)]);
  });
  return added;
}
