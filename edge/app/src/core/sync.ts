// SF5L8: Edge -> Backend upload. The phone sends each record's EXACT sealed
// frame (newest first, so the backend map shows "now" soon) plus what it saw
// locally (first arrival, the Gateway's RX signal, attached sessions). The
// backend answers per record; only STORED / DUPLICATE / CONFLICT / REJECTED
// are final. UNCERTAIN or any network/HTTP failure leaves the record
// pending, so it is sent again. Nothing is deleted on the phone.
// Protocol: backend/orun_backend_bench.py (POST /v1/edge/batch).
import { hex } from "./bytes.ts";
import type { Db, Row } from "./db.ts";
import type { ReadKey } from "./keys.ts";

export const BATCH = 100;
export const SESSIONS_MAX = 64;
const FINAL = new Set(["STORED", "DUPLICATE", "CONFLICT", "REJECTED"]);

export interface HttpReply { status: number; body: unknown; }
export type Post = (url: string, token: string, body: string) => Promise<HttpReply>;
export type Get = (url: string, token: string) => Promise<HttpReply>;
export type Exclusive = <T>(fn: () => Promise<T>) => Promise<T>;

export interface UploadCounts {
  uploaded: number;   // STORED + DUPLICATE on the backend
  pending: number;    // not answered finally yet
  conflict: number;
  rejected: number;
}

export interface RunResult { sent: number; final: number; uncertain: number; }

export class Uploader {
  private db: Db;
  private post: Post;
  private exclusive: Exclusive;
  private clock: () => number;

  constructor(db: Db, post: Post, exclusive: Exclusive, clock: () => number = () => Date.now() / 1000) {
    this.db = db; this.post = post; this.exclusive = exclusive; this.clock = clock;
  }

  private async meta(key: string): Promise<string | null> {
    const r = await this.db.query("SELECT value FROM meta WHERE key=?", [key]);
    return r.length ? String(r[0].value) : null;
  }

  private async setMeta(key: string, value: string): Promise<void> {
    await this.db.run("INSERT OR REPLACE INTO meta (key,value) VALUES (?,?)", [key, value]);
  }

  /** This phone's stable Edge id (random, created once). */
  edgeId(): Promise<string> {
    return this.exclusive(async () => {
      const have = await this.meta("edge_id");
      if (have) return have;
      const id = "phone-" + hex(crypto.getRandomValues(new Uint8Array(6))).toLowerCase();
      await this.setMeta("edge_id", id);
      return id;
    });
  }

  counts(): Promise<UploadCounts> {
    return this.exclusive(async () => {
      const total = Number((await this.db.query("SELECT count(*) AS n FROM observations"))[0].n);
      const by = new Map((await this.db.query("SELECT state, count(*) AS n FROM uploads GROUP BY state"))
        .map((r) => [String(r.state), Number(r.n)]));
      const answered = [...by.values()].reduce((a, b) => a + b, 0);
      return {
        uploaded: (by.get("STORED") ?? 0) + (by.get("DUPLICATE") ?? 0),
        pending: Math.max(0, total - answered),
        conflict: by.get("CONFLICT") ?? 0,
        rejected: by.get("REJECTED") ?? 0,
      };
    });
  }

  /** One batch, optionally scanning only older rows in this drain pass.
   * UNCERTAIN rows remain pending but are not retried ahead of older records
   * within the same drain. A later drain retries them normally. */
  private async runBatch(url: string, token: string, beforeRowid: number | null):
    Promise<{ result: RunResult; nextRowid: number | null }> {
    const edge = await this.edgeId();
    const { rows, sessions, lastSession } = await this.exclusive(async () => {
      const rows = await this.db.query(
        "SELECT o.rowid AS scan_rowid,o.device_id,o.incarnation,o.sequence,o.frame,r.first_seen_utc,r.gw_rssi,r.gw_snr,r.gateway " +
        "FROM observations o LEFT JOIN receipts r ON r.device_id=o.device_id AND r.incarnation=o.incarnation AND r.sequence=o.sequence " +
        "LEFT JOIN uploads u ON u.device_id=o.device_id AND u.incarnation=o.incarnation AND u.sequence=o.sequence " +
        "WHERE u.state IS NULL " + (beforeRowid === null ? "" : "AND o.rowid < ? ") +
        "ORDER BY o.rowid DESC LIMIT ?", beforeRowid === null ? [BATCH] : [beforeRowid, BATCH]);
      // The last sent session keeps growing while attached: resend from it.
      const lastSession = Number((await this.meta("sessions_from")) ?? "1");
      const sessions = await this.db.query(
        "SELECT id,started_utc,alive_utc,transport FROM sessions WHERE id>=? ORDER BY id LIMIT ?",
        [lastSession, SESSIONS_MAX]);
      return { rows, sessions, lastSession };
    });
    const sessionsChanged = await this.exclusive(async () =>
      sessions.length > 0 && JSON.stringify(sessions) !== (await this.meta("sessions_sent")));
    const nextRowid = rows.length ? Number(rows[rows.length - 1].scan_rowid) : null;
    if (!rows.length && !sessionsChanged)
      return { result: { sent: 0, final: 0, uncertain: 0 }, nextRowid: null };

    const num = (v: Row[string]) => (v === null || v === undefined ? null : Number(v));
    const body = JSON.stringify({
      edge,
      records: rows.map((r) => ({
        frame: String(r.frame), gateway: r.gateway === null ? null : String(r.gateway),
        first_seen_utc: num(r.first_seen_utc), rssi: num(r.gw_rssi), snr: num(r.gw_snr),
      })),
      sessions: sessions.map((s) => ({
        id: Number(s.id), started_utc: Number(s.started_utc), alive_utc: Number(s.alive_utc),
        transport: s.transport === "ble" || s.transport === "usb" ? s.transport : null,
      })),
    });
    const reply = await this.post(url.replace(/\/+$/, "") + "/v1/edge/batch", token, body);
    if (reply.status === 401) throw new Error("sunucu anahtarı (token) yanlış");
    if (reply.status !== 200) throw new Error(`sunucu cevabı HTTP ${reply.status}`);
    const out = reply.body as { results?: unknown; sessions_ok?: unknown };
    if (!out || !Array.isArray(out.results) || out.results.length !== rows.length)
      throw new Error("sunucu cevabı anlaşılamadı");
    const results = out.results.map(String);

    const now = Math.floor(this.clock());
    let final = 0;
    await this.exclusive(() => this.db.transaction(async () => {
      for (let i = 0; i < rows.length; i++) {
        if (!FINAL.has(results[i])) continue;      // UNCERTAIN: send again later
        await this.db.run(
          "INSERT OR REPLACE INTO uploads (device_id,incarnation,sequence,state,utc) VALUES (?,?,?,?,?)",
          [rows[i].device_id, rows[i].incarnation, rows[i].sequence, results[i], now]);
        final++;
      }
      if (out.sessions_ok === true && sessions.length) {
        await this.setMeta("sessions_from", String(Math.max(lastSession, Number(sessions[sessions.length - 1].id))));
        await this.setMeta("sessions_sent", JSON.stringify(sessions));
      }
    }));
    return { result: { sent: rows.length, final, uncertain: rows.length - final }, nextRowid };
  }

  /** A one-shot upload always starts with the newest pending records. */
  async runOnce(url: string, token: string): Promise<RunResult> {
    return (await this.runBatch(url, token, null)).result;
  }

  /** An empty batch: checks address and token without sending records. */
  async ping(url: string, token: string): Promise<number | null> {
    const edge = await this.edgeId();
    const reply = await this.post(url.replace(/\/+$/, "") + "/v1/edge/batch", token,
                                  JSON.stringify({ edge, records: [], sessions: [] }));
    if (reply.status === 401) throw new Error("sunucu anahtarı (token) yanlış");
    if (reply.status !== 200) throw new Error(`sunucu cevabı HTTP ${reply.status}`);
    const out = reply.body as { results?: unknown; records?: unknown };
    if (!out || !Array.isArray(out.results) || out.results.length !== 0)
      throw new Error("sunucu cevabı anlaşılamadı");
    return typeof out.records === "number" ? out.records : null;
  }

  /** One newest-to-oldest pass. Each pending record is attempted at most
   * once per drain, even if the backend returns UNCERTAIN for an entire batch.
   * Those records stay pending and will be retried on the next sync tick. */
  async drain(url: string, token: string): Promise<RunResult> {
    const total: RunResult = { sent: 0, final: 0, uncertain: 0 };
    let beforeRowid: number | null = null;
    for (let i = 0; i < 1000; i++) {
      const { result: r, nextRowid } = await this.runBatch(url, token, beforeRowid);
      total.sent += r.sent; total.final += r.final; total.uncertain += r.uncertain;
      if (r.sent < BATCH || nextRowid === null) break;
      beforeRowid = nextRowid;
    }
    return total;
  }
}

/** Step 3A / D1: this owner's Tracker read keys from the backend. */
export async function fetchReadKeys(get: Get, url: string, token: string): Promise<ReadKey[]> {
  const reply = await get(url.replace(/\/+$/, "") + "/v1/edge/keys", token);
  if (reply.status === 401) throw new Error("sunucu anahtarı (token) yanlış");
  if (reply.status !== 200) throw new Error(`sunucu cevabı HTTP ${reply.status}`);
  const keys = (reply.body as { keys?: unknown })?.keys;
  if (!Array.isArray(keys)) throw new Error("sunucu cevabı anlaşılamadı");
  return keys.map((k) => ({
    device: String((k as Record<string, unknown>).device ?? ""),
    epoch: Number((k as Record<string, unknown>).epoch),
    trafficKey: String((k as Record<string, unknown>).traffic_key ?? ""),
  }));
}
