// v1 step 7 (Android): the Gateway link lives in the native bridge service,
// not in this WebView, so it keeps receiving and handing off records with
// the app closed or swiped away. This class is the app's view of it: the
// same surface as BleLink (start/stop/redump + LinkEvents), plus taking the
// bridge's records over into the app's ledger and carrying LoRa command
// lines while the app runs.
import type { Db } from "../core/db.ts";
import { importRows, readCursor } from "../core/bridgeImport.ts";
import type { ReadKey } from "../core/keys.ts";
import { LoraCarrier, type LoraServer } from "../app/loraCarrier.ts";
import type { BridgeStatus, GatewayBridgePlugin } from "./gatewayBridge.ts";
import type { LinkEvents, LinkState } from "./transports.ts";

export interface NativeLinkHooks {
  /** Every status from the bridge (before the link state is reported). */
  status(s: BridgeStatus): void;
  /** n rows were new to the app's ledger (or upload states moved). */
  imported(n: number): void;
}

type Exclusive = <T>(fn: () => Promise<T>) => Promise<T>;

const PAGE = 200, PAGES = 50, LORA_MS = 2000;

function errorText(e: unknown): string { return e instanceof Error ? e.message : String(e); }

/** The bridge's state as the app's link state (+ the line shown under it). */
export function linkStateOf(s: BridgeStatus): { state: LinkState; detail: string | null } {
  if (!s.enabled) return { state: "idle", detail: null };
  switch (s.state) {
    case "ready": return { state: "connected", detail: s.keyMissing ? "Teslim anahtarı yok; kayıtlar gateway’de korunuyor." : null };
    case "connecting": return { state: "connecting", detail: s.detail };
    case "adapter_off": return { state: "waiting", detail: s.detail ?? "Bluetooth kapalı" };
    case "waiting": return { state: "waiting", detail: s.detail ?? "Gateway'e ulaşılamıyor" };
    default: return { state: "connecting", detail: s.detail ?? "köprü başlatılıyor" };
  }
}

export class NativeBleLink {
  private listening = false;
  private importing = false;
  private importAgain = false;
  private refreshing = false;
  private refreshAgain = false;
  private lora = new LoraCarrier();
  private loraSeen: number | null = null;
  private loraTimer: ReturnType<typeof setInterval> | null = null;
  private ready = false;
  last: BridgeStatus | null = null;

  private readonly bridge: GatewayBridgePlugin;
  private readonly db: Db;
  private readonly exclusive: Exclusive;
  private readonly ev: LinkEvents;
  private readonly hooks: NativeLinkHooks;
  private readonly pickAddress: (pick: boolean, deviceId?: string) => Promise<string>;
  private readonly loraServer: () => LoraServer | null;

  constructor(bridge: GatewayBridgePlugin, db: Db, exclusive: Exclusive, ev: LinkEvents, hooks: NativeLinkHooks,
              pickAddress: (pick: boolean, deviceId?: string) => Promise<string>,
              loraServer: () => LoraServer | null) {
    this.bridge = bridge; this.db = db; this.exclusive = exclusive; this.ev = ev; this.hooks = hooks;
    this.pickAddress = pickAddress; this.loraServer = loraServer;
  }

  /** App start: a bridge that kept running while the app was closed shows at
   * once, and its records come over. Returns whether it is on. */
  async attach(): Promise<boolean> {
    await this.listen();
    const s = await this.bridge.status();
    // LoRa lines from before this app run were for an earlier carrier.
    this.loraSeen = s.loraSeq ?? 0;
    if (s.enabled) await this.bridge.resume().catch(() => undefined);
    this.apply(s);
    void this.importAll();
    return s.enabled;
  }

  async start(pick = false, deviceId?: string): Promise<void> {
    try {
      const address = (await this.pickAddress(pick, deviceId)).toUpperCase();
      await this.listen();
      this.ev.state("connecting");
      await this.bridge.start({ address });
      this.ev.log(`[köprü] gateway seçildi: ${address}; bağlantıyı telefonun köprü servisi tutar`);
      await this.refresh();
    } catch (e) {
      this.ev.state("idle", errorText(e));
      this.ev.log(`[BLE] başlatılamadı: ${errorText(e)}`);
    }
  }

  async stop(): Promise<void> {
    await this.bridge.stop().catch(() => undefined);
    this.stopLora();
    await this.refresh();
    this.ev.state("idle");
  }

  /** Signed out: the bridge stops and drops this account's keys. */
  async forget(): Promise<void> {
    await this.bridge.forget().catch(() => undefined);
    this.stopLora();
    await this.refresh();
  }

  /** New read keys: the bridge asks the Gateway again for what it could not open. */
  async redump(): Promise<void> { await this.bridge.redump().catch(() => undefined); }

  /** The app's read keys (and a reload of the Edge-key vault) for the bridge. */
  async pushKeys(keys: ReadKey[]): Promise<void> { await this.bridge.setReadKeys({ keys }); }

  private async listen(): Promise<void> {
    if (this.listening) return;
    this.listening = true;
    await this.bridge.addListener("change", () => { void this.refresh(); });
  }

  /** Coalesces bursts of "change" (a Gateway dump is hundreds of records). */
  async refresh(): Promise<void> {
    if (this.refreshing) { this.refreshAgain = true; return; }
    this.refreshing = true;
    try {
      do {
        this.refreshAgain = false;
        const s = await this.bridge.status();
        this.apply(s);
        await this.loraIn(s);
        void this.importAll();
      } while (this.refreshAgain);
    } catch (e) {
      this.ev.log(`[köprü] durum alınamadı: ${errorText(e)}`);
    } finally {
      this.refreshing = false;
    }
  }

  private logSeen: string | null = null;

  /** The bridge's own log lines (it runs while the app is closed) into the
   * app's technical log, each once. */
  private logIn(s: BridgeStatus): void {
    const lines = s.log ?? [];
    if (!lines.length) return;
    const at = this.logSeen === null ? -1 : lines.lastIndexOf(this.logSeen);
    for (const line of lines.slice(at + 1)) this.ev.log(`[köprü] ${line}`);
    this.logSeen = lines[lines.length - 1];
  }

  private apply(s: BridgeStatus): void {
    this.last = s;
    this.logIn(s);
    this.hooks.status(s);
    const { state, detail } = linkStateOf(s);
    const ready = state === "connected";
    if (ready && !this.ready) { this.lora.reset(); this.startLora(); }
    if (!ready && this.ready) this.stopLora();
    this.ready = ready;
    this.ev.state(state, detail ?? undefined);
    this.ev.changed();
  }

  /** Takes every row the bridge has that the app's ledger has not. */
  async importAll(): Promise<void> {
    if (this.importing) { this.importAgain = true; return; }
    this.importing = true;
    try {
      for (let page = 0; page < PAGES; page++) {
        const cur = await this.exclusive(() => readCursor(this.db));
        const p = await this.bridge.takeRecords({ db: cur.db, after: cur.rev, limit: PAGE });
        if (!p.rows.length) break;
        const n = await this.exclusive(() => importRows(this.db, p.db, p.rows, p.last));
        await this.bridge.taken({ db: p.db, rev: p.last });
        this.hooks.imported(n);
        if (p.rows.length < PAGE) break;
      }
    } catch (e) {
      this.ev.log(`[köprü] kayıtlar uygulamaya alınamadı: ${errorText(e)}; sonra yeniden denenecek`);
    } finally {
      this.importing = false;
      if (this.importAgain) { this.importAgain = false; void this.importAll(); }
    }
  }

  private async loraIn(s: BridgeStatus): Promise<void> {
    if (this.loraSeen === null || s.loraSeq === undefined || s.loraSeq <= this.loraSeen) return;
    const r = await this.bridge.takeLora({ after: this.loraSeen });
    for (const line of r.lines) this.lora.observe(line);
    this.loraSeen = r.last;
  }

  private startLora(): void {
    this.stopLora();
    this.loraTimer = setInterval(() => {
      void this.lora.poll(this.loraServer(), (line) => this.bridge.sendLine({ line }), Date.now());
    }, LORA_MS);
  }

  private stopLora(): void {
    if (this.loraTimer) clearInterval(this.loraTimer);
    this.loraTimer = null;
  }
}
