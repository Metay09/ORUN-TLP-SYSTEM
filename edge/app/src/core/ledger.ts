// Phone ledger of verified observations. Port of VerifiedLocalLedger
// (backend/candidates/product_secure_verified_ledger.py): AEAD verified,
// content-aware dedup, conflicts kept as evidence, readback after commit.
// A record is acknowledged to the Gateway only for STORED or DUPLICATE.
import { hex, fromHex } from "./bytes.ts";
import type { Db } from "./db.ts";
import { openProductFrame, Reject, type CredentialLookup, type Observation } from "./product.ts";

export type LedgerState = "STORED" | "DUPLICATE" | "CONFLICT" | "REJECTED" | "UNCERTAIN";

export interface LedgerResult { state: LedgerState; item: Observation | null; }

export const MAX_CONFLICTS = 4096;

export class Ledger {
  private readonly db: Db;
  private readonly credentials: CredentialLookup;

  constructor(db: Db, credentials: CredentialLookup) {
    this.db = db;
    this.credentials = credentials;
  }

  async ingest(raw: Uint8Array): Promise<LedgerResult> {
    let item: Observation;
    try {
      item = openProductFrame(raw, this.credentials);
    } catch (e) {
      if (e instanceof Reject) return { state: "REJECTED", item: null };
      throw e;
    }
    const key = [hex(item.deviceId), hex(item.incarnation), item.sequence];
    const plain = hex(item.plaintext);
    try {
      const { state, conflict } = await this.db.transaction(async () => {
        const saved = await this.db.query(
          "SELECT family,plaintext FROM observations WHERE device_id=? AND incarnation=? AND sequence=?",
          key);
        if (!saved.length) {
          await this.db.run(
            "INSERT INTO observations (device_id,incarnation,sequence,family,plaintext,frame) VALUES (?,?,?,?,?,?)",
            [...key, item.family, plain, hex(item.frame)]);
          return { state: "STORED" as LedgerState, conflict: false };
        }
        if (Number(saved[0].family) === item.family && saved[0].plaintext === plain) {
          // A known divergence quarantines the whole identity: never report
          // a clean duplicate (and so never accept) while one exists.
          const c = await this.db.query(
            "SELECT 1 AS x FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? LIMIT 1",
            key);
          return { state: (c.length ? "CONFLICT" : "DUPLICATE") as LedgerState, conflict: false };
        }
        const prior = await this.db.query(
          "SELECT 1 AS x FROM conflicts WHERE device_id=? AND incarnation=? AND sequence=? AND family=? AND plaintext=?",
          [...key, item.family, plain]);
        if (!prior.length) {
          const n = await this.db.query("SELECT count(*) AS n FROM conflicts");
          if (Number(n[0].n) >= MAX_CONFLICTS) throw new Error("conflict capacity");
          await this.db.run(
            "INSERT INTO conflicts (device_id,incarnation,sequence,family,plaintext,frame) VALUES (?,?,?,?,?,?)",
            [...key, item.family, plain, hex(item.frame)]);
        }
        return { state: "CONFLICT" as LedgerState, conflict: true };
      });
      if (!(await this.matches(item, conflict))) return { state: "UNCERTAIN", item };
      return { state, item };
    } catch {
      return { state: "UNCERTAIN", item };
    }
  }

  // After COMMIT: the stored FIRST frame must authenticate back to the same
  // logical content. A damaged database never yields a duplicate success.
  private async matches(item: Observation, conflict: boolean): Promise<boolean> {
    const table = conflict ? "conflicts" : "observations";
    let sql = `SELECT family,plaintext,frame FROM ${table} WHERE device_id=? AND incarnation=? AND sequence=?`;
    const args: (string | number)[] = [hex(item.deviceId), hex(item.incarnation), item.sequence];
    if (conflict) { sql += " AND family=? AND plaintext=?"; args.push(item.family, hex(item.plaintext)); }
    const rows = await this.db.query(sql, args);
    if (!rows.length || Number(rows[0].family) !== item.family ||
        rows[0].plaintext !== hex(item.plaintext))
      return false;
    try {
      const stored = openProductFrame(fromHex(String(rows[0].frame)), this.credentials);
      return hex(stored.deviceId) === hex(item.deviceId) &&
        hex(stored.incarnation) === hex(item.incarnation) &&
        stored.sequence === item.sequence && stored.family === item.family &&
        hex(stored.plaintext) === hex(item.plaintext);
    } catch {
      return false;
    }
  }

  async recordCount(): Promise<number | null> {
    try {
      const r = await this.db.query("SELECT count(*) AS n FROM observations");
      return Number(r[0].n);
    } catch {
      return null;
    }
  }
}
