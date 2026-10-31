// Owns the phone ledger, the Edge core and the two links; the UI subscribes.
import { hex64 } from "../core/bytes.ts";
import type { Db } from "../core/db.ts";
import { EdgeCore, type Counts } from "../core/edge.ts";
import { KeyRing } from "../core/keys.ts";
import type { PointsResult } from "../core/points.ts";
import { BleLink, UsbLink, type LinkState } from "../platform/transports.ts";
import { fetchReadKeys, Uploader, type UploadCounts } from "../core/sync.ts";
import { fetchGet, fetchPost } from "../platform/http.ts";
import { KeepAlive } from "../platform/background.ts";
import { AccountSharedMirror } from "./accountSharedMirror.ts";
import { attachPilotAccountSqliteMirror, pilotAccountUploadTransport } from "./ownerAccountPilotRuntime.ts";

export interface Snapshot {
  ready: boolean;
  error: string | null;
  link: "none" | "ble" | "usb";
  state: LinkState;
  detail: string | null;
  counts: Counts;
  records: number | null;
  handedOff: number;
  rejected: number;
  pending: number;
  gateway: string | null;
  gatewayHeld: string | null;
  storage: string;
  log: string[];
  server: ServerState;
  pointsVersion: number;     // bumps whenever the ledger may have new points
  journal: JournalEntry[];   // "Defter": plain-language history, newest last
}

// One line of the plain-language history (Defter). Technical detail stays in
// the raw log.
export type JournalKind = "link" | "records" | "handoff" | "upload" | "keys" | "warn";
export interface JournalEntry { at: number; kind: JournalKind; text: string; n?: number; }

export interface ServerState {
  url: string;
  token: string;
  auto: boolean;
  busy: boolean;
  counts: UploadCounts | null;
  lastOk: string | null;     // local time of the last successful upload
  error: string | null;
}

const SERVER_KEY = "orun.edge.server";
const KEYS_EVERY_MS = 10 * 60 * 1000;   // read keys rarely change
const SYNC_EVERY_MS = 30000;   // and soon after new records arrive
const SYNC_AFTER_RECORD_MS = 5000;

function loadServer(): Pick<ServerState, "url" | "token" | "auto"> {
  // Old observation bearer was persisted by previous APK versions. It must
  // not survive into account-only login, nor be used after account logout.
  try { localStorage.removeItem(SERVER_KEY); } catch { /* storage unavailable */ }
  return { url: "", token: "", auto: true };
}

const LOG_MAX = 400;
const JOURNAL_KEY = "orun.edge.journal";
const JOURNAL_MAX = 300;
const COALESCE_MS = 3 * 60 * 1000;

function loadJournal(): JournalEntry[] {
  try {
    const v = JSON.parse(localStorage.getItem(JOURNAL_KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((e) => e && typeof e.text === "string").slice(-JOURNAL_MAX) : [];
  } catch {
    return [];
  }
}

function heldText(held: number | null, capacity: number | null): string | null {
  if (held === null) return null;
  return capacity ? `${held}/${capacity}` : `${held}`;
}

export class Controller {
  readonly kind = "real" as const;
  private core: EdgeCore | null = null;
  private ble: BleLink | null = null;
  private usb: UsbLink | null = null;
  private listeners = new Set<() => void>();
  private refreshQueued = false;
  private keepAlive = new KeepAlive((l) => this.log(l));
  snap: Snapshot = {
    ready: false, error: null, link: "none", state: "idle", detail: null,
    counts: { STORED: 0, DUPLICATE: 0, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 },
    records: null, handedOff: 0, rejected: 0, pending: 0, gateway: null, gatewayHeld: null,
    storage: "", log: [],
    server: { ...loadServer(), busy: false, counts: null, lastOk: null, error: null },
    pointsVersion: 0, journal: loadJournal(),
  };
  private lastStored = 0;
  private lastHanded = 0;
  private serverDown = false;
  private uploader: Uploader | null = null;
  private keyring = new KeyRing();
  private db: Db | null = null;
  private keysAt = 0;
  private syncTimer: ReturnType<typeof setTimeout> | null = null;
  private lastRecords: number | null = null;
  // Lock before the first await; timer/manual calls must not upload concurrently.
  private syncing = false;

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  }

  private emit(patch: Partial<Snapshot>): void {
    this.snap = { ...this.snap, ...patch };
    for (const fn of this.listeners) fn();
    this.background();
  }

  // While a link is up (or waiting to reconnect) the app keeps running with
  // the screen off; the notification shows the transfer state.
  private background(): void {
    const s = this.snap;
    if (s.link === "none") { this.keepAlive.off(); return; }
    const how = s.link === "ble" ? "Bluetooth" : "USB";
    const body = s.state === "connected"
      ? `${how}: ${s.counts.STORED} yeni · teslim ${s.handedOff} · bekleyen ${s.pending}`
      : `${how}: yeniden bağlanmayı bekliyor`;
    this.keepAlive.on(body);
  }

  async start(openDb: () => Promise<Db & { journal?: string; synchronous?: string }>): Promise<void> {
    try {
      const db = await openDb();
      this.db = db;
      const keyring = this.keyring;
      const core = new EdgeCore(db, { credentials: keyring.lookup });
      await core.init();
      await keyring.load(db);
      this.core = core;
      // The account mirror uses THIS existing SQLite Db. Every SQL operation
      // is queued behind BLE/USB EdgeCore work. HTTP happens outside the queue.
      // Registering does NOT create tables, fetch records or activate sync.
      attachPilotAccountSqliteMirror(new AccountSharedMirror(
        db, job => core.exclusive(job)));
      const events = {
        log: (line: string) => this.log(line),
        state: (s: LinkState, detail?: string) => {
          const was = this.snap.state;
          this.emit({ state: s, detail: detail ?? null, ...(s === "idle" ? { link: "none" as const } : {}) });
          if (s === "connected" && was !== "connected")
            this.note("link", `İstasyona bağlanıldı (${this.snap.link === "usb" ? "USB" : "Bluetooth"})`);
          if (was === "connected" && s !== "connected")
            this.note("link", s === "idle" ? "İstasyon bağlantısı kapatıldı" : "İstasyon bağlantısı koptu · yeniden deneniyor");
        },
        changed: () => this.refresh(),
      };
      this.ble = new BleLink(core, events);
      this.usb = new UsbLink(core, events);
      this.emit({ ready: true, storage: `SQLite journal=${db.journal ?? "?"} synchronous=${db.synchronous ?? "?"}` });
      this.uploader = new Uploader(db, fetchPost, (fn) => core.exclusive(fn));
      this.refresh();
      setInterval(() => { void this.sync(false); }, SYNC_EVERY_MS);
      void this.sync(false);
    } catch (e) {
      this.emit({ error: e instanceof Error ? e.message : String(e) });
    }
  }

  /** Ledger points for the map/herd views (null until the ledger is open). */
  async points(): Promise<PointsResult | null> {
    return this.core ? this.core.points() : null;
  }

  /** Adds (or, for bursts, updates) one plain-language Defter line. */
  note(kind: JournalKind, text: string, n?: number): void {
    const journal = [...this.snap.journal];
    const last = journal[journal.length - 1];
    const now = Date.now();
    if (n !== undefined && last && last.kind === kind && last.n !== undefined && now - last.at < COALESCE_MS) {
      const total = last.n + n;
      journal[journal.length - 1] = { at: now, kind, n: total, text: text.replace(/^\d+/, String(total)) };
    } else {
      journal.push({ at: now, kind, text, ...(n !== undefined ? { n } : {}) });
    }
    const kept = journal.slice(-JOURNAL_MAX);
    try { localStorage.setItem(JOURNAL_KEY, JSON.stringify(kept)); } catch { /* memory only */ }
    this.emit({ journal: kept });
  }

  log(line: string): void {
    const stamp = new Date().toLocaleTimeString("tr-TR", { hour12: false });
    const log = [...this.snap.log, ...line.split("\n").map((l) => `${stamp} ${l}`)];
    this.emit({ log: log.slice(-LOG_MAX) });
  }

  // Coalesces bursts (a full pass is hundreds of records) into one UI update.
  refresh(): void {
    if (this.refreshQueued || !this.core) return;
    this.refreshQueued = true;
    setTimeout(async () => {
      this.refreshQueued = false;
      const c = this.core!;
      const stored = c.counts.STORED, handed = c.handedOff;
      if (stored > this.lastStored) this.note("records", `${stored - this.lastStored} yeni kayıt istasyondan alındı`, stored - this.lastStored);
      if (handed > this.lastHanded) this.note("handoff", `${handed - this.lastHanded} kayıt telefona kalıcı yazıldı · istasyonda yer açıldı`, handed - this.lastHanded);
      this.lastStored = stored;
      this.lastHanded = handed;
      this.emit({
        pointsVersion: this.snap.pointsVersion + 1,
        counts: { ...c.counts }, records: await c.ledger.recordCount(), handedOff: c.handedOff,
        rejected: c.acceptRejected, pending: c.pendingAccepts(),
        gateway: c.gatewayId === null ? this.snap.gateway : hex64(c.gatewayId),
        // Live: the HELLO count, then what the Gateway announced minus what it
        // confirmed handed off (it reports its count only once per connection).
        gatewayHeld: heldText(c.heldNow(), c.lastHello?.capacity ?? null) ?? this.snap.gatewayHeld,
      });
      // New records: upload soon (newest first), not only on the 30 s tick.
      if (this.snap.records !== this.lastRecords) {
        this.lastRecords = this.snap.records;
        if (!this.syncTimer)
          this.syncTimer = setTimeout(() => { this.syncTimer = null; void this.sync(false); },
                                      SYNC_AFTER_RECORD_MS);
      }
    }, 250);
  }

  // ---- backend upload (SF5L8) ----------------------------------------------
  private setServer(patch: Partial<ServerState>): void {
    this.emit({ server: { ...this.snap.server, ...patch } });
  }

  saveServer(url: string, token: string, auto: boolean): void {
    const clean = url.trim().replace(/\/+$/, "");
    try { localStorage.setItem(SERVER_KEY, JSON.stringify({ url: clean, token: token.trim(), auto })); } catch { /* memory only */ }
    this.setServer({ url: clean, token: token.trim(), auto, error: null });
    void this.sync(true);
  }

  // Step 3A / D1: this owner's Tracker read keys. New keys -> ask the
  // Gateway for its held records again: those it could not open before
  // were never accepted, so they are still there.
  private async refreshKeys(url: string, token: string): Promise<void> {
    const core = this.core, db = this.db;
    if (!core || !db) return;
    const keys = await fetchReadKeys(fetchGet, url, token);
    const changed = await core.exclusive(() => this.keyring.store(db, keys));
    this.keysAt = Date.now();
    if (changed) {
      this.log(`[sunucu] ${changed} okuma anahtarı alındı (tasma kayıtları artık açılabilir)`);
      this.note("keys", `${changed} tasmanın okuma anahtarı sunucudan alındı`);
      if (this.snap.link === "ble") await this.ble?.redump();
      if (this.snap.link === "usb") await this.usb?.redump();
    }
  }

  /** Uploads pending records; manual=true runs even with automatic upload off. */
  async sync(manual: boolean): Promise<void> {
    // Counts, key refresh and HTTP calls all await. Guard the entire sequence,
    // rather than setting server.busy only after the first two awaits.
    if (!this.uploader || this.syncing || this.snap.server.busy) return;
    this.syncing = true;
    try {
      const sv = this.snap.server;
      // Credentials are account scoped, in memory, and never saved in
      // localStorage. No independent address/token entry is necessary.
      const authenticated = pilotAccountUploadTransport();
      const url = authenticated?.url ?? "";
      const token = authenticated?.token ?? "";
      const counts = await this.uploader.counts();
      if (!url || !token || (!manual && !sv.auto)) {
        this.setServer({ counts });
        return;
      }
      this.setServer({ busy: true, counts });

      // Fetching new read keys helps decode future Gateway records. Already
      // committed, verified phone records do not depend on this request and
      // must still be uploaded when the key endpoint is temporarily down.
      let keysError: string | null = null;
      if (manual || Date.now() - this.keysAt > KEYS_EVERY_MS) {
        try {
          await this.refreshKeys(url, token);
        } catch (e) {
          keysError = `okuma anahtarları alınamadı: ${e instanceof Error ? e.message : String(e)}`;
          if (manual) this.log(`[sunucu] okuma anahtarları alınamadı: ${keysError}; mevcut kayıtlar yine gönderilecek`);
        }
      }
      if (!manual && counts.pending === 0) {
        this.setServer({ busy: false, counts, error: keysError });
        return;
      }

      try {
        // Nothing to send: a manual tap still checks address + token.
        if (counts.pending === 0) {
          const n = await this.uploader.ping(url, token);
          this.setServer({ busy: false, counts, error: keysError,
                           lastOk: new Date().toLocaleTimeString("tr-TR", { hour12: false }) });
          this.log(`[sunucu] bağlantı tamam; sunucudaki toplam kayıt ${n ?? "?"}`);
          return;
        }
        const r = await this.uploader.drain(url, token);
        const after = await this.uploader.counts();
        this.setServer({
          busy: false, counts: after, error: keysError,
          lastOk: new Date().toLocaleTimeString("tr-TR", { hour12: false }),
        });
        if (r.final) {
          this.log(`[sunucu] ${r.final} kayıt gönderildi; sunucuda ${after.uploaded}, bekleyen ${after.pending}`);
          this.note("upload", `${r.final} kayıt sunucuya gönderildi`, r.final);
        }
        this.serverDown = false;
        if (r.uncertain) this.log(`[sunucu] ${r.uncertain} kayıt için sunucu emin değil; tekrar gönderilecek`);
        if (after.conflict || after.rejected)
          this.log(`UYARI sunucu: çakışma ${after.conflict}, red ${after.rejected}`);
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        const counts2 = await this.uploader.counts();
        this.setServer({ busy: false, error: msg, counts: counts2 });
        if (manual) this.log(`[sunucu] gönderilemedi: ${msg}`);
        if (!this.serverDown && counts2.pending) {
          this.serverDown = true;
          this.note("warn", `Sunucuya ulaşılamadı · ${counts2.pending} kayıt telefonda saklı, sonra gönderilecek`);
        }
      }
    } finally {
      this.syncing = false;
      if (this.snap.server.busy) this.setServer({ busy: false });
    }
  }

  async connectBle(pick = false): Promise<void> {
    if (!this.ble || this.snap.link !== "none") return;
    this.emit({ link: "ble" });
    await this.ble.start(pick);
  }

  async connectUsb(): Promise<void> {
    if (!this.usb || this.snap.link !== "none") return;
    this.emit({ link: "usb" });
    await this.usb.start();
  }

  async disconnect(): Promise<void> {
    if (this.snap.link === "ble") await this.ble?.stop();
    if (this.snap.link === "usb") await this.usb?.stop();
    this.emit({ link: "none", state: "idle", detail: null });
    this.refresh();
  }
}
