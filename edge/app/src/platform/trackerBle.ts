// 4a: tasmaya (ya da herhangi bir ORUN cihazına) Bluetooth ile bağlanıp ORUN
// uygulama servisinden durum okur. Bir soru, bir yanıt; yanıt gelmeden yenisi
// gönderilmez (cihaz böyle ister).
import { BleClient } from "@capacitor-community/bluetooth-le";
import {
  APP_REQUEST_UUID, APP_RESPONSE_UUID, APP_SERVICE_UUID, AppReassembler, QUERY,
  requestFrames, type AppMessage, type Family,
} from "../app/bleAppFrames.ts";
import { decodeBle, FAMILIES, type Raw } from "../app/trackerStatus.ts";
import { stopNearbyScan } from "./bleScan.ts";

const ANSWER_MS = 4000;

export class TrackerBle {
  private rx = new AppReassembler();
  private waiting: ((m: AppMessage) => void) | null = null;
  private correlation = Math.floor(Math.random() * 0xffff);
  private gone = false;
  readonly deviceId: string;

  private constructor(deviceId: string) { this.deviceId = deviceId; }

  static async open(deviceId: string): Promise<TrackerBle> {
    const t = new TrackerBle(deviceId);
    await BleClient.initialize({ androidNeverForLocation: true });
    await stopNearbyScan();
    await BleClient.connect(deviceId, () => { t.gone = true; }, { timeout: 15000 });
    try {
      await BleClient.startNotifications(deviceId, APP_SERVICE_UUID, APP_RESPONSE_UUID, (v) => {
        const m = t.rx.push(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
        if (m && t.waiting) t.waiting(m);
      });
    } catch (e) {
      try { await BleClient.disconnect(deviceId); } catch { /* gone */ }
      throw new Error("Bu cihazda ORUN uygulama servisi yok ya da kapalı: " + (e instanceof Error ? e.message : String(e)));
    }
    return t;
  }

  /** Scan for the device whose Bluetooth name ends with the last 8 digits of `id`. */
  static async find(id: string, ms = 10000): Promise<string | null> {
    const want = "ORUN-" + id.slice(-8).toUpperCase();
    await BleClient.initialize({ androidNeverForLocation: true });
    if (!(await BleClient.isEnabled())) await BleClient.requestEnable();
    await stopNearbyScan();
    let hit: string | null = null;
    await BleClient.requestLEScan({ namePrefix: "ORUN-", allowDuplicates: false }, (r) => {
      if ((r.localName ?? r.device.name ?? "").toUpperCase() === want) hit = r.device.deviceId;
    });
    const start = Date.now();
    while (!hit && Date.now() - start < ms) await new Promise((r) => setTimeout(r, 200));
    await stopNearbyScan();
    return hit;
  }

  /** One request (fragmented if it has data), one answer. */
  readonly exchange = async (type: number, payload: Uint8Array, waitMs = ANSWER_MS): Promise<AppMessage> => {
    if (this.gone) throw new Error("Bluetooth bağlantısı koptu.");
    this.correlation = (this.correlation + 1) & 0xffff;
    const corr = this.correlation;
    this.rx.reset();
    const answer = new Promise<AppMessage | null>((resolve) => {
      const timer = setTimeout(() => { this.waiting = null; resolve(null); }, waitMs);
      this.waiting = (m) => { if (m.correlation !== corr) return; clearTimeout(timer); this.waiting = null; resolve(m); };
    });
    for (const f of requestFrames(type, corr, payload))
      await BleClient.write(this.deviceId, APP_SERVICE_UUID, APP_REQUEST_UUID, new DataView(f.buffer, f.byteOffset, f.byteLength));
    const m = await answer;
    if (!m) throw new Error("Cihaz yanıt vermedi.");
    return m;
  };

  private ask(family: Family): Promise<AppMessage> {
    return this.exchange(QUERY[family], new Uint8Array(0));
  }

  /** Every family the device answers; one that fails is left out. */
  async readRaw(families: Family[] = FAMILIES): Promise<Raw> {
    const raw: Raw = {};
    for (const f of families) {
      try { raw[f] = decodeBle(f, await this.ask(f)); }
      catch (e) { if (this.gone) throw e; }
    }
    return raw;
  }

  async close(): Promise<void> {
    this.waiting = null;
    try { await BleClient.stopNotifications(this.deviceId, APP_SERVICE_UUID, APP_RESPONSE_UUID); } catch { /* gone */ }
    try { await BleClient.disconnect(this.deviceId); } catch { /* gone */ }
  }
}
