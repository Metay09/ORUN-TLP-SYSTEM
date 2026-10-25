// Owns the phone ledger, the Edge core and the two links; the UI subscribes.
import { hex64 } from "../core/bytes.ts";
import type { Db } from "../core/db.ts";
import { EdgeCore, type Counts } from "../core/edge.ts";
import { benchCredentials } from "../core/product.ts";
import type { PointsResult } from "../core/points.ts";
import { BleLink, UsbLink, type LinkState } from "../platform/transports.ts";
import { KeepAlive } from "../platform/background.ts";

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
}

const LOG_MAX = 400;

export class Controller {
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
  };

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
      const core = new EdgeCore(db, { credentials: benchCredentials });
      await core.init();
      this.core = core;
      const events = {
        log: (line: string) => this.log(line),
        state: (s: LinkState, detail?: string) =>
          this.emit({ state: s, detail: detail ?? null, ...(s === "idle" ? { link: "none" as const } : {}) }),
        changed: () => this.refresh(),
      };
      this.ble = new BleLink(core, events);
      this.usb = new UsbLink(core, events);
      this.emit({ ready: true, storage: `SQLite journal=${db.journal ?? "?"} synchronous=${db.synchronous ?? "?"}` });
      this.refresh();
      // Expose the ledger to the map page (iframe, same origin).
      (window as unknown as { orunPoints: () => Promise<PointsResult> }).orunPoints = () => core.points();
    } catch (e) {
      this.emit({ error: e instanceof Error ? e.message : String(e) });
    }
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
      this.emit({
        counts: { ...c.counts }, records: await c.ledger.recordCount(), handedOff: c.handedOff,
        rejected: c.acceptRejected, pending: c.pendingAccepts(),
        gateway: c.gatewayId === null ? this.snap.gateway : hex64(c.gatewayId),
        gatewayHeld: c.lastHello ? `${c.lastHello.held}/${c.lastHello.capacity}` : this.snap.gatewayHeld,
      });
    }, 250);
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
