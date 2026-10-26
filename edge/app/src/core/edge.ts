// The phone's Edge work, transport neutral. Port of BenchEdge
// (tools/orun_edge_bench.py): ledger ingest, receipts, sessions, and the
// confirmed hand-off queue (one accept in flight; BUSY or 3 s silence ->
// send again; OK/UNKNOWN -> done; REJECTED/FAIL -> reported, record stays on
// the Gateway). An accept is queued only after the ledger COMMIT returned.
import { hex, hex64 } from "./bytes.ts";
import type { Db } from "./db.ts";
import { SCHEMA } from "./db.ts";
import {
  ACCEPT_WORDS, acceptLine, acceptMessage, decodeGatewayMessage, UsbLineParser,
} from "./edgeLink.ts";
import { Ledger, type LedgerState } from "./ledger.ts";
import { describePeriodic, human, type CredentialLookup, type Observation } from "./product.ts";
import { buildPoints, type PointsResult } from "./points.ts";

export const ACCEPT_TIMEOUT_S = 3;

type Link = { rssi: number; snr: number } | null;
interface Pending { key: string; page: number; slot: number; line: string; binary: Uint8Array; }

export interface EdgeOptions {
  credentials: CredentialLookup;
  accept?: boolean;               // send confirmed hand-offs (default true)
  clock?: () => number;           // seconds since epoch
}

export type Counts = Record<"STORED" | "DUPLICATE" | "REJECTED" | "CONFLICT" | "UNCERTAIN", number>;

export class EdgeCore {
  readonly ledger: Ledger;
  readonly counts: Counts = { STORED: 0, DUPLICATE: 0, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 };
  handedOff = 0;
  acceptRejected = 0;
  gatewayId: bigint | null = null;
  lastHello: { held: number; capacity: number } | null = null;
  // What the Gateway holds right now, as far as this session can tell. The
  // Gateway reports its count only in HELLO (once per connection); after its
  // first full pass (DUMP END) every held record has been announced, so the
  // count is exactly those announced minus those it confirmed handed off.
  private heldKeys = new Set<string>();     // "page:slot": last known survivors
  // Count distinct HELD announcements independently of ACCEPT/reclaim. A
  // missing BLE fragment or a broken USB line must not make DUMP END look like
  // proof that the Gateway has fewer records than it reported.
  private dumpSeen = new Set<string>();
  private passDone = false;
  private incompleteFloor = 0;
  private okSinceHello = 0;
  private readonly accept: boolean;
  private readonly clock: () => number;
  private usb = new UsbLineParser();
  private queue: Pending[] = [];
  private queued = new Set<string>();
  private inFlight: (Pending & { sent: number }) | null = null;
  private session: number | null = null;
  // Every Gateway event is applied strictly in arrival order.
  private chain: Promise<unknown> = Promise.resolve();

  private readonly db: Db;

  constructor(db: Db, opts: EdgeOptions) {
    this.db = db;
    this.ledger = new Ledger(db, opts.credentials);
    this.accept = opts.accept ?? true;
    this.clock = opts.clock ?? (() => Date.now() / 1000);
  }

  async init(): Promise<void> {
    for (const s of SCHEMA) await this.db.execute(s);
  }

  /** Runs fn with exclusive use of the database, after every earlier event. */
  exclusive<T>(fn: () => Promise<T>): Promise<T> { return this.serial(fn); }

  private serial<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.chain.then(fn, fn);
    this.chain = next.catch(() => undefined);
    return next;
  }

  // ---- sessions ----------------------------------------------------------
  /** A Gateway is attached and being read right now (call every ~15 s). */
  sessionAlive(transport: string): Promise<void> {
    return this.serial(async () => {
      const now = Math.floor(this.clock());
      if (this.session === null) {
        await this.db.run("INSERT INTO sessions (started_utc,alive_utc,transport) VALUES (?,?,?)",
                          [now, now, transport]);
        const r = await this.db.query("SELECT max(id) AS id FROM sessions");
        this.session = Number(r[0].id);
      } else {
        await this.db.run("UPDATE sessions SET alive_utc=? WHERE id=?", [now, this.session]);
      }
    });
  }

  /** The link dropped: the next attach is a new session, nothing in flight. */
  sessionEnd(): Promise<void> {
    return this.serial(async () => {
      if (this.session !== null)
        await this.db.run("UPDATE sessions SET alive_utc=? WHERE id=?",
                          [Math.floor(this.clock()), this.session]);
      this.session = null;
      // The next connection re-sends every record the Gateway still holds,
      // so pending accepts are rebuilt from that pass; start clean.
      this.inFlight = null;
      this.queue = [];
      this.queued.clear();
      this.gatewayId = null;
      this.usb = new UsbLineParser();
      // Keep the last known count for the screen until the next HELLO.
      const held = this.heldNow();
      if (this.lastHello && held !== null) this.lastHello = { ...this.lastHello, held };
      this.heldKeys.clear();
      this.dumpSeen.clear();
      this.passDone = false;
      this.incompleteFloor = 0;
      this.okSinceHello = 0;
    });
  }

  // ---- accept queue ------------------------------------------------------
  private queueAccept(gateway: bigint, page: number, slot: number, gen: bigint,
                      raw: Uint8Array): void {
    const key = `${page}:${slot}:${gen}`;
    if (!this.accept || this.queued.has(key)) return;
    this.queued.add(key);
    this.queue.push({
      key, page, slot,
      line: acceptLine(gateway, page, slot, gen, raw),
      binary: acceptMessage(gateway, page, slot, gen, raw),
    });
  }

  private next(now: number): Pending | null {
    if (this.inFlight) {
      if (now - this.inFlight.sent < ACCEPT_TIMEOUT_S) return null;
      this.queue.unshift(this.inFlight);     // no reply: send again
      this.inFlight = null;
    }
    const p = this.queue.shift();
    if (!p) return null;
    this.inFlight = { ...p, sent: now };
    return p;
  }

  /** Next USB accept line to write, if any (one in flight at a time). */
  nextAcceptLine(now = this.clock()): string | null { return this.next(now)?.line ?? null; }
  /** Next BLE ACCEPT message to write, if any. */
  nextAcceptMessage(now = this.clock()): Uint8Array | null { return this.next(now)?.binary ?? null; }

  pendingAccepts(): number { return this.queue.length + (this.inFlight ? 1 : 0); }

  /** Exact after a complete dump; otherwise a conservative known count. */
  heldNow(): number | null {
    if (this.passDone) return this.heldKeys.size;
    if (!this.lastHello && this.incompleteFloor === 0) return null;
    return Math.max(this.heldKeys.size, this.incompleteFloor,
                    this.lastHello ? Math.max(0, this.lastHello.held - this.okSinceHello) : 0);
  }

  /** Start a new requested scan; do not mix earlier live HELD events into its count. */
  beginDump(): Promise<void> {
    return this.serial(async () => {
      this.dumpSeen.clear();
      this.passDone = false;
      this.incompleteFloor = 0;
    });
  }

  private finishDump(count: number): boolean {
    // DUMP END claims that count HELD records were announced. Count every
    // unique announcement, including already accepted/reclaimed records.
    const complete = Number.isSafeInteger(count) && count >= 0 && this.dumpSeen.size === count;
    this.passDone = complete;
    this.incompleteFloor = complete ? 0 :
      (Number.isSafeInteger(count) && count >= 0 ? Math.max(0, count - this.okSinceHello) : 0);
    this.dumpSeen.clear();
    return complete;
  }

  private acceptReply(word: string, page: number | null, slot: number | null): string | null {
    const f = this.inFlight;
    if (!f) return null;
    if (page === null || word === "BUSY") {
      this.queue.unshift(f);                 // store busy: retry
      this.inFlight = null;
      return null;
    }
    if (page !== f.page || slot !== f.slot) return null;
    this.inFlight = null;
    this.queued.delete(f.key);
    if (word === "OK" || word === "UNKNOWN") {
      // UNKNOWN: already reclaimed (or never held): nothing left to keep.
      if (word === "OK") { this.handedOff++; this.okSinceHello++; }
      this.heldKeys.delete(`${f.page}:${f.slot}`);
      return null;
    }
    this.acceptRejected++;
    return `UYARI gateway teslimi reddetti page=${f.page} slot=${f.slot} (${word}); kayıt gateway'de kalır`;
  }

  // ---- ingest --------------------------------------------------------------
  private async receipt(item: Observation, link: Link, gateway: bigint): Promise<void> {
    const key = [hex(item.deviceId), hex(item.plaintext.subarray(4, 12)), item.sequence];
    // First sighting wins: a later dump is not a new arrival.
    await this.db.run(
      "INSERT OR IGNORE INTO receipts (device_id,incarnation,sequence,first_seen_utc,gw_rssi,gw_snr,gateway) VALUES (?,?,?,?,?,?,?)",
      [...key, Math.floor(this.clock()), link?.rssi ?? null, link?.snr ?? null, hex64(gateway)]);
    if (link)
      await this.db.run(
        "UPDATE receipts SET gw_rssi=?, gw_snr=? WHERE device_id=? AND incarnation=? AND sequence=? AND gw_rssi IS NULL",
        [link.rssi, link.snr, ...key]);
  }

  private async ingestHeld(gateway: bigint, page: number, slot: number, gen: bigint,
                           raw: Uint8Array, link: Link): Promise<string> {
    const place = `${page}:${slot}`;
    this.heldKeys.add(place);
    this.dumpSeen.add(place);
    const { state, item } = await this.ledger.ingest(raw);
    this.counts[state]++;
    const word: Record<LedgerState, string> = {
      STORED: "YENİ ", DUPLICATE: "VARDI", REJECTED: "RED  ", CONFLICT: "ÇAKIŞ", UNCERTAIN: "BELİRSİZ",
    };
    if (state === "REJECTED" || state === "UNCERTAIN" || !item)
      return `${word[state]} gw=${hex64(gateway)} page=${page} slot=${slot}`;
    try {
      await this.receipt(item, link, gateway);
    } catch {
      // Arrival time / signal are map facts; the record itself is committed.
    }
    if (state === "STORED" || state === "DUPLICATE") this.queueAccept(gateway, page, slot, gen, raw);
    const sig = link ? ` | sinyal ${link.rssi} dBm SNR ${link.snr}` : "";
    if (item.family !== 1) return `${word[state]} tasma=${hex(item.deviceId)} seq=${item.sequence} (EVENT)${sig}`;
    return `${word[state]} ${human(describePeriodic(item.deviceId, item.plaintext))}${sig}`;
  }

  /** One serial line from a USB-attached Gateway. */
  handleLine(text: string): Promise<string | null> {
    return this.serial(async () => {
      const e = this.usb.line(text);
      if (!e) return null;
      switch (e.type) {
        case "acceptReply": return this.acceptReply(e.word, e.page, e.slot);
        case "reclaim":
          if (e.what === "OK") {
            this.forgetPage(e.page);
            return `[gateway] sayfa ${e.page} boşaltıldı (kayıtları teslim edilmişti)`;
          }
          if (e.what === "FAIL") return `UYARI gateway sayfa ${e.page} silinemedi`;
          return null;
        case "dumpEnd": {
          const match = /\bcount=(\d+)\b/.exec(e.text);
          const complete = this.finishDump(match ? Number(match[1]) : NaN);
          return `[gateway] ${e.text} | ${await this.summary()}` +
            (complete ? "" : " | UYARI: eksik/bozuk aktarım; sayaç kesin değil, yeniden DUMP gerekli");
        }
        case "heldBroken":
          this.counts.REJECTED++;
          return `RED   gw=${e.gateway} page=${e.page} slot=${e.slot}: satır eksik/bozuk (${e.got} hex, beklenen ${e.want}); BENCH GW DUMP ile tekrar istenebilir`;
        case "held":
          this.gatewayId = e.gateway;
          return this.ingestHeld(e.gateway, e.page, e.slot, e.generation, e.object, e.link);
      }
    });
  }

  /** One complete edge_link message from a BLE-attached Gateway. */
  handleMessage(msg: Uint8Array): Promise<string | null> {
    return this.serial(async () => {
      const m = decodeGatewayMessage(msg);
      if (!m) return null;
      switch (m.type) {
        case "hello":
          this.gatewayId = m.gateway;
          this.lastHello = { held: m.held, capacity: m.capacity };
          this.heldKeys.clear();
          this.dumpSeen.clear();
          this.passDone = false;
          this.incompleteFloor = 0;
          this.okSinceHello = 0;
          return `[gateway BLE] ${hex64(m.gateway)} bağlı; tuttuğu kayıt ${m.held}/${m.capacity}`;
        case "held":
          if (this.gatewayId === null) return null;   // HELLO comes first; never guess
          return this.ingestHeld(this.gatewayId, m.page, m.slot, m.generation, m.object, m.link);
        case "dumpEnd": {
          const complete = this.finishDump(m.count);
          return `[gateway BLE] DUMP END count=${m.count} | ${await this.summary()}` +
            (complete ? "" : " | UYARI: eksik aktarım; sayaç kesin değil, yeniden DUMP gerekli");
        }
        case "acceptResult":
          return this.acceptReply(ACCEPT_WORDS[m.code] ?? "REJECTED", m.page, m.slot);
        case "reclaimed":
          if (m.ok) this.forgetPage(m.page);
          return m.ok ? `[gateway] sayfa ${m.page} boşaltıldı (kayıtları teslim edilmişti)`
                      : `UYARI gateway sayfa ${m.page} silinemedi`;
      }
    });
  }

  private forgetPage(page: number): void {
    for (const k of this.heldKeys) if (k.startsWith(`${page}:`)) this.heldKeys.delete(k);
  }

  async summary(): Promise<string> {
    const c = this.counts;
    return `ÖZET yeni=${c.STORED} vardı=${c.DUPLICATE} red=${c.REJECTED} ` +
      `çakışma=${c.CONFLICT} belirsiz=${c.UNCERTAIN} toplam_kayıt=${await this.ledger.recordCount()}` +
      (this.accept ? ` teslim=${this.handedOff} teslim_red=${this.acceptRejected} bekleyen_teslim=${this.pendingAccepts()}` : "");
  }

  /** The map's data (same JSON as the PC tool's /api/points). */
  points(): Promise<PointsResult> {
    return this.serial(() => buildPoints(this.db, this.clock()));
  }
}
