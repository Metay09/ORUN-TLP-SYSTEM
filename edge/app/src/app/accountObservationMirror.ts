// Account-scoped durable mirror of SERVER observations (not the phone Edge ledger).
// No Gateway ACCEPT, no upload queue, no command execution and no local coverage
// claim. Every exact frame is independently AEAD-verified BEFORE the SQLite
// cursor can advance. All pages are atomic with readback after COMMIT.
import type { Db } from "../core/db.ts";
import { fromHex, hex } from "../core/bytes.ts";
import {
  describePeriodic, openProductFrame, type CredentialLookup,
} from "../core/product.ts";
import type { Point } from "../core/points.ts";
import type { OwnerAccountClient, AccountIdentity } from "./ownerAccountClient.ts";
import type { ServerObservationPage, ServerObservationRow } from "./serverObservationRead.ts";
import type { Observation } from "../core/product.ts";

const CURSORS = "orun_account_observation_cursors_v1";
const ROWS = "orun_account_observation_records_v1";
// One automatic sync round; the next round continues from the saved cursor.
const MAX_PAGES = 64;
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const MIN_EXP = () => Date.now() / 1000;

interface Verified { raw: ServerObservationRow; item: Observation }

export interface ObservationMirrorResult {
  received: number;
  pages: number;
  cursor: number;
  more: boolean;
}

function identity(client: OwnerAccountClient): {
  actor: AccountIdentity; epoch: number;
} {
  const actor = client.current;
  if (!actor || !ID.test(actor.site) || !ID.test(actor.actor) ||
      !ID.test(actor.installation) || !ID.test(actor.username) ||
      !Number.isSafeInteger(actor.expiresAt) || actor.expiresAt <= MIN_EXP())
    throw new Error("Önce geçerli hesabınla giriş yapmalısın.");
  return { actor, epoch: client.sessionEpoch };
}
function still(client: OwnerAccountClient, actor: AccountIdentity, epoch: number): boolean {
  const current = client.current;
  return client.sessionEpoch === epoch && !!current &&
    current.site === actor.site && current.actor === actor.actor &&
    current.installation === actor.installation &&
    current.username === actor.username &&
    current.expiresAt === actor.expiresAt && current.expiresAt > MIN_EXP();
}
function keys(actor: AccountIdentity): string[] {
  return [actor.site, actor.actor, actor.installation];
}

export class AccountObservationMirror {
  private ready = false;
  private busy = false;
  private readonly db: Db;
  private readonly lookup: CredentialLookup;
  private readonly exclusive: <T>(work: () => Promise<T>) => Promise<T>;

  constructor(
    db: Db,
    lookup: CredentialLookup,
    exclusive: <T>(work: () => Promise<T>) => Promise<T>,
  ) {
    this.db = db;
    this.lookup = lookup;
    this.exclusive = exclusive;
  }

  async init(): Promise<void> {
    if (this.ready) return;
    await this.exclusive(async () => {
      await this.db.execute(`CREATE TABLE IF NOT EXISTS ${CURSORS} (
        site TEXT NOT NULL, actor TEXT NOT NULL, installation TEXT NOT NULL,
        cursor INTEGER NOT NULL CHECK(cursor>=0),
        PRIMARY KEY(site,actor,installation)
      )`);
      await this.db.execute(`CREATE TABLE IF NOT EXISTS ${ROWS} (
        site TEXT NOT NULL, actor TEXT NOT NULL, installation TEXT NOT NULL,
        device_id TEXT NOT NULL, incarnation TEXT NOT NULL,
        sequence INTEGER NOT NULL, family INTEGER NOT NULL,
        plaintext TEXT NOT NULL, frame TEXT NOT NULL,
        first_seen_utc INTEGER, rssi INTEGER, snr INTEGER,
        PRIMARY KEY(site,actor,installation,device_id,incarnation,sequence)
      )`);
    });
    this.ready = true;
  }

  private async cursor(actor: AccountIdentity): Promise<number> {
    const rows = await this.db.query(
      `SELECT cursor FROM ${CURSORS} WHERE site=? AND actor=? AND installation=?`,
      keys(actor));
    if (rows.length > 1) throw new Error("Kayıt imleci çakışıyor.");
    if (!rows.length) return 0;
    const n = rows[0].cursor;
    if (typeof n !== "number" || !Number.isSafeInteger(n) || n < 0)
      throw new Error("Sunucu gözlem imleci bozuk; korunuyor.");
    return n;
  }

  /** AEAD-verify server rows. A 200 response does not make a frame trusted. */
  private verify(rows: ServerObservationRow[]): Verified[] {
    return rows.map(r => {
      const item = openProductFrame(fromHex(r.frame), this.lookup);
      if (hex(item.deviceId) !== r.device ||
          hex(item.incarnation) !== r.incarnation ||
          item.sequence !== r.sequence || item.family !== r.family)
        throw new Error("Sunucu kaydı ile şifreli içerik uyuşmuyor.");
      return { raw: r, item };
    });
  }

  /** Inside a transaction: insert new rows, accept identical repeats, refuse
   * conflicting content. Returns how many rows were new. */
  private async insert(actor: AccountIdentity, verified: Verified[]): Promise<number> {
    let added = 0;
    for (const { raw, item } of verified) {
      const recordKey = [...keys(actor), raw.device, raw.incarnation, raw.sequence];
      const existing = await this.db.query(
        `SELECT plaintext,frame FROM ${ROWS}
         WHERE site=? AND actor=? AND installation=? AND
               device_id=? AND incarnation=? AND sequence=?`,
        recordKey);
      const plain = hex(item.plaintext);
      const frame = hex(item.frame);
      if (existing.length > 1 ||
          (existing.length === 1 &&
           (existing[0].plaintext !== plain || existing[0].frame !== frame)))
        throw new Error("Sunucuda kayıt içerik çakışması var.");
      if (existing.length === 0) {
        await this.db.run(
          `INSERT INTO ${ROWS}
          (site,actor,installation,device_id,incarnation,sequence,family,
           plaintext,frame,first_seen_utc,rssi,snr)
          VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`,
          [...recordKey, raw.family, plain, frame, raw.first_seen_utc, raw.rssi, raw.snr],
        );
        added++;
      }
    }
    return added;
  }

  private async guarded<T>(work: () => Promise<T>): Promise<T> {
    if (this.busy) throw new Error("Konum indirme zaten sürüyor.");
    this.busy = true;
    try {
      await this.init();
      return await work();
    } finally {
      this.busy = false;
    }
  }

  /** Newest measured position of every tracker, first. Stored like any other
   * verified record; the history cursor does not move, so later history pages
   * still arrive and identical repeats are accepted. */
  async pullLatest(client: OwnerAccountClient): Promise<{ received: number; added: number }> {
    return this.guarded(async () => {
      const { actor, epoch } = identity(client);
      const latest = await client.readServerLatest();
      if (!still(client, actor, epoch))
        throw new Error("Hesap değişti; konum kaydedilmedi.");
      const verified = this.verify(latest.records);
      const added = await this.exclusive(() => this.db.transaction(async () => {
        if (!still(client, actor, epoch))
          throw new Error("Hesap değişti; kayıt yazılmadı.");
        const n = await this.insert(actor, verified);
        if (!still(client, actor, epoch))
          throw new Error("Hesap değişti; SQLite yazımı geri alındı.");
        return n;
      }));
      return { received: verified.length, added };
    });
  }

  async pullPage(client: OwnerAccountClient): Promise<{
    received: number; cursor: number; more: boolean;
  }> {
    return this.guarded(async () => {
      const { actor, epoch } = identity(client);
      const previous = await this.exclusive(() => this.cursor(actor));
      const page: ServerObservationPage = await client.readServerObservations(previous);
      if (!still(client, actor, epoch))
        throw new Error("Hesap değişti; konum kaydedilmedi.");
      const verified = this.verify(page.records);

      if (page.cursor !== previous) {
        await this.exclusive(() => this.db.transaction(async () => {
          if (await this.cursor(actor) !== previous)
            throw new Error("Kayıt imleci başka işlemde değişti.");
          if (!still(client, actor, epoch))
            throw new Error("Hesap değişti; kayıt yazılmadı.");
          await this.insert(actor, verified);
          await this.db.run(
            `INSERT INTO ${CURSORS} (site,actor,installation,cursor)
             VALUES(?,?,?,?)
             ON CONFLICT(site,actor,installation) DO UPDATE SET cursor=excluded.cursor`,
            [...keys(actor), page.cursor]);
          if (!still(client, actor, epoch))
            throw new Error("Hesap değişti; SQLite yazımı geri alındı.");
        }));
        const persisted = await this.exclusive(() => this.cursor(actor));
        if (persisted !== page.cursor)
          throw new Error("Sunucu kayıt imleci SQLite'da doğrulanamadı.");
      }
      return { received: verified.length, cursor: page.cursor, more: page.has_more };
    });
  }

  /** Up to maxPages history pages from the persisted cursor (resumable). */
  async pullAvailable(client: OwnerAccountClient, maxPages = MAX_PAGES): Promise<ObservationMirrorResult> {
    if (!Number.isSafeInteger(maxPages) || maxPages < 1 || maxPages > MAX_PAGES)
      throw new Error("Konum indirme sınırı geçersiz.");
    let received = 0, pages = 0, cursor = 0, more = false;
    for (let i = 0; i < maxPages; i++) {
      const next = await this.pullPage(client);
      received += next.received;
      cursor = next.cursor;
      more = next.more;
      pages++;
      if (!more) break;
    }
    return { received, pages, cursor, more };
  }

  /** Offline-safe: never exposes one user's history under another account. */
  async points(client: OwnerAccountClient): Promise<Point[]> {
    await this.init();
    const { actor, epoch } = identity(client);
    const rows = await this.exclusive(() => this.db.query(
      `SELECT device_id,incarnation,sequence,family,plaintext,frame,first_seen_utc,rssi,snr
       FROM ${ROWS} WHERE site=? AND actor=? AND installation=?
       ORDER BY device_id,incarnation,sequence`,
      keys(actor)));
    if (!still(client, actor, epoch))
      throw new Error("Hesap değişti; konumlar gösterilmedi.");
    const result: Point[] = [];
    for (const r of rows) {
      if (r.family !== 1) continue;
      // Revalidate persisted content to avoid silently rendering a corrupted
      // or cross-account SQLite row.
      if (typeof r.device_id !== "string" || typeof r.plaintext !== "string" ||
          typeof r.frame !== "string" || typeof r.incarnation !== "string" ||
          typeof r.sequence !== "number") throw new Error("Konum aynası bozuk.");
      const restored = openProductFrame(fromHex(r.frame), this.lookup);
      if (hex(restored.deviceId) !== r.device_id ||
          hex(restored.incarnation) !== r.incarnation ||
          restored.sequence !== r.sequence ||
          hex(restored.plaintext) !== r.plaintext)
        throw new Error("Kalıcı konum kaydı doğrulanamadı.");
      const d = describePeriodic(restored.deviceId, restored.plaintext);
      if (d.incarnation !== r.incarnation || d.sequence !== r.sequence)
        throw new Error("Konum aynası kimliği uyuşmuyor.");
      if (d.lat === null || d.lon === null) continue;
      const seen = typeof r.first_seen_utc === "number" ? r.first_seen_utc : null;
      const recorded = d.time ? Date.parse(d.time) / 1000 : null;
      result.push({
        tracker: d.tracker, incarnation: d.incarnation, seq: d.sequence,
        time: d.time, lat: d.lat, lon: d.lon, alt_m: d.alt_m,
        sats: d.sats, hdop: d.hdop, fix_age_s: d.fix_age_s,
        battery_mv: d.battery_mv, battery: d.battery, active_s: d.active_s,
        covered_s: d.covered_s, source: d.source, period_s: d.period_s,
        rssi: typeof r.rssi === "number" ? r.rssi : null,
        snr: typeof r.snr === "number" ? r.snr : null,
        received: seen ? new Date(seen * 1000).toISOString() : null,
        delay_s: seen && recorded !== null ? Math.max(0, Math.floor(seen - recorded)) : null,
        link: null, delivery: "server",
      });
    }
    return result;
  }
}
