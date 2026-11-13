// Pil ömrü (sahip isteği 2026-11-11; model 2026-11-13).
//
//   ortalama akım = bekleme akımı + rapor başına maliyet × saatteki rapor
//
// İki sayı tasmanın kendi pil düşüşünden öğrenilir (fitDrain): pille geçen,
// rapor aralığı sabit dönemlerde harcanan mAh. İki ayrı aralıkta dönem varsa
// ikisi de ölçülür; tek aralık varsa biri varsayılandan alınır. Hiç dönem
// yoksa varsayılanlar kullanılır ve ekranda "tahmin" yazar.
// Güneş katkısı katılmaz (9. adım). mAh her tasma için ayrı girilir.
//
// Eski hesap (üretici akımları, "işlemci uyumuyor: taban 2,2 mA") sahada
// tutmadı: Hayvan 2, 2 dk aralıkta 9,7 mA yerine ~4,4 mA harcadı (2026-11-13).
import { batteryPercentExact } from "./herd.ts";

/** Where nothing was measured on this collar. */
export const DRAIN_DEFAULT = {
  /** Per report: GPS start + send + 10 s listen. Hayvan 2 (450 mAh), 2-minute reports, 21 h, 2026-11-13. */
  packetMah: 0.15,
  /** Between reports. Not measured yet (same bench bounds it to 0.05-0.3 mA); published sleep figure is 0.03. */
  baseMa: 0.15,
};
const BASE_MIN_MA = 0.02, PACKET_MIN_MAH = 0.02;
/** u-blox ZOE-M8Q continuous: 25 mA. Under 60 s the GNSS stays on (gnss_config kShortIntervalThreshold). */
const GNSS_ON_MA = 25, SHORT_INTERVAL_S = 60;
/** A LiPo is not run to 0 %; count 80 % of the label as usable. */
export const USABLE = 0.8;

export interface Reading { t: number; mv: number; power?: string | null; usb?: boolean | null; periodS?: number | null; }
/** One stretch on the battery alone at one report interval. */
export interface Run { hours: number; packets: number; mAh: number; periodS: number; }
export interface Drain {
  baseMa: number; packetMah: number;
  /** both: two intervals measured · one: one of the two is the default · none: defaults. */
  measured: "both" | "one" | "none";
  hours: number;
}

const HOUR_MS = 3_600_000;
const RUN_MIN_H = 6, RUN_MIN_POINTS = 8, SETTLE_MS = HOUR_MS;   // first hour off the charger: surface charge
const median = (v: number[]) => { const s = [...v].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };

/** Battery-only stretches with a constant report interval, as hours / reports / mAh used. */
export function drainRuns(series: Reading[], mAh: number): Run[] {
  const runs: Run[] = [];
  let cur: Reading[] = [], offSince = 0;
  const close = () => {
    const pts = cur; cur = [];
    if (pts.length < RUN_MIN_POINTS) return;
    const k = Math.max(1, Math.min(5, Math.floor(pts.length / 3)));
    const head = pts.slice(0, k), tail = pts.slice(-k);
    const hours = (median(tail.map((x) => x.t)) - median(head.map((x) => x.t))) / HOUR_MS;
    const used = ((median(head.map((x) => batteryPercentExact(x.mv))) - median(tail.map((x) => batteryPercentExact(x.mv)))) / 100) * mAh;
    const periodS = pts[0].periodS!;
    // A battery that did not fall (solar, unnoticed charging) measures nothing.
    if (hours >= RUN_MIN_H && used > 0) runs.push({ hours, packets: (hours * 3600) / periodS, mAh: used, periodS });
  };
  for (const r of [...series].sort((a, b) => a.t - b.t)) {
    if (r.usb === true || r.power === "NO_BATTERY" || r.power === "FULL") { close(); offSince = 0; continue; }
    if (!r.periodS || r.periodS <= 0) continue;
    if (!offSince) offSince = r.t;
    if (cur.length && cur[0].periodS !== r.periodS) close();
    if (r.t - offSince < SETTLE_MS) continue;
    cur.push(r);
  }
  close();
  return runs;
}

/** Standby current and per-report cost from the runs (least squares, weighted by length). */
export function fitDrain(runs: Run[]): Drain {
  const D = DRAIN_DEFAULT;
  if (!runs.length) return { baseMa: D.baseMa, packetMah: D.packetMah, measured: "none", hours: 0 };
  let hh = 0, kk = 0, hk = 0, hm = 0, km = 0, H = 0, K = 0, M = 0;
  for (const r of runs) {
    hh += r.hours * r.hours; kk += r.packets * r.packets; hk += r.hours * r.packets;
    hm += r.hours * r.mAh; km += r.packets * r.mAh; H += r.hours; K += r.packets; M += r.mAh;
  }
  const rates = runs.map((r) => r.packets / r.hours);
  const det = hh * kk - hk * hk;
  if (Math.max(...rates) >= 2 * Math.min(...rates) && det > 0) {
    const baseMa = (hm * kk - km * hk) / det, packetMah = (km * hh - hm * hk) / det;
    if (baseMa >= BASE_MIN_MA && packetMah >= PACKET_MIN_MAH) return { baseMa, packetMah, measured: "both", hours: H };
  }
  // One interval (or two that disagree): only the term that dominated the
  // drain can be read from it; the other stays at its default. Reading the
  // standby current off 2-minute reports would turn noise into months.
  if (D.packetMah * K > D.baseMa * H * 1.001)
    return { baseMa: D.baseMa, packetMah: Math.max(PACKET_MIN_MAH, (M - D.baseMa * H) / K), measured: "one", hours: H };
  return { baseMa: Math.max(BASE_MIN_MA, (M - D.packetMah * K) / H), packetMah: D.packetMah, measured: "one", hours: H };
}

export interface Life { avgMa: number; mAhPerDay: number; daysFull: number; daysLeft: number | null; }

/** Average current, days on a full battery and days left at one report every `intervalS`. */
export function lifeAt(mAh: number, intervalS: number, d: Pick<Drain, "baseMa" | "packetMah">, nowPct: number | null = null): Life {
  const avgMa = d.baseMa + (d.packetMah * 3600) / intervalS + (intervalS < SHORT_INTERVAL_S ? GNSS_ON_MA : 0);
  const mAhPerDay = avgMa * 24;
  return {
    avgMa, mAhPerDay, daysFull: (mAh * USABLE) / mAhPerDay,
    daysLeft: nowPct !== null ? Math.max(0, ((mAh * nowPct) / 100 - mAh * (1 - USABLE)) / mAhPerDay) : null,
  };
}

/** "~30 saat", "~12 gün", "~5 ay", "~2 yıl". */
export function lifeWord(days: number): string {
  if (!Number.isFinite(days) || days <= 0) return "—";
  if (days < 2) return `~${Math.max(1, Math.round(days * 24))} saat`;
  if (days < 60) return `~${Math.round(days)} gün`;
  if (days < 730) return `~${Math.round(days / 30)} ay`;
  return `~${(days / 365).toFixed(1).replace(".", ",")} yıl`;
}

// ---- pil kapasitesi, tasma başına, hesapta --------------------------------
// Kapasiteyi tasma kullanmaz; yalnız pil ömrü hesabı içindir. Bu yüzden
// tasmaya yazılmaz, adı gibi hesapta durur (sahip kararı 2026-11-11), bütün
// telefonlarda aynı. Telefonda bir kopya; internetsiz verilen değer sonra gider.
type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.batteryMah.v2";
const OLD_KEY = "orun.batteryMah.v1";  // yalnız telefonda tutulan eski değerler
const ID16 = /^[0-9A-F]{16}$/;
export const MAH_MIN = 50, MAH_MAX = 50_000;

export interface BatteryServer {
  batteryCapacities(): Promise<Record<string, number>>;
  setBatteryCapacity(device: string, mAh: number | null): Promise<void>;
}

const okMah = (x: unknown): x is number => Number.isInteger(x) && (x as number) >= MAH_MIN && (x as number) <= MAH_MAX;

function validValues(v: unknown): Record<string, number> {
  const out: Record<string, number> = {};
  if (!v || typeof v !== "object") return out;
  for (const [k, x] of Object.entries(v as Record<string, unknown>)) if (ID16.test(k) && okMah(x)) out[k] = x;
  return out;
}

export class BatteryCapacityBook {
  private values: Record<string, number> = {};
  /** Sent to the account yet? null = delete. */
  private pending: Record<string, number | null> = {};
  private fns = new Set<() => void>();
  private store: Store | null;
  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      if (raw) {
        const v = JSON.parse(raw) as { values?: unknown; pending?: unknown };
        this.values = validValues(v.values);
        if (v.pending && typeof v.pending === "object")
          for (const [k, x] of Object.entries(v.pending as Record<string, unknown>))
            if (ID16.test(k) && (x === null || okMah(x))) this.pending[k] = x as number | null;
      } else {
        // Önceki sürüm yalnız telefonda tutuyordu: hepsi hesaba gönderilecek.
        this.values = validValues(JSON.parse(store?.getItem(OLD_KEY) ?? "{}"));
        this.pending = { ...this.values };
      }
    } catch { /* start empty */ }
  }
  private save(): void {
    try { this.store?.setItem(KEY, JSON.stringify({ values: this.values, pending: this.pending })); } catch { /* memory only */ }
    for (const f of this.fns) f();
  }
  subscribe = (fn: () => void): (() => void) => { this.fns.add(fn); return () => { this.fns.delete(fn); }; };
  snapshot = (): Readonly<Record<string, number>> => this.values;
  get(device: string): number | null { return this.values[device.toUpperCase()] ?? null; }
  /** Shows at once; sends now, or later when there is no account/internet. */
  async set(device: string, mAh: number | null, server: BatteryServer | null = null): Promise<void> {
    const id = device.toUpperCase();
    if (!ID16.test(id)) throw new Error("Cihaz kimliği geçersiz.");
    if (mAh !== null && !okMah(mAh)) throw new Error(`Kapasite ${MAH_MIN}–${MAH_MAX} mAh arasında olmalı.`);
    const next = { ...this.values };
    if (mAh === null) delete next[id]; else next[id] = mAh;
    this.values = next;
    this.pending = { ...this.pending, [id]: mAh };
    this.save();
    if (server) await this.push(server, id);
  }
  private async push(server: BatteryServer, id: string): Promise<void> {
    if (!(id in this.pending)) return;
    const v = this.pending[id];
    try {
      await server.setBatteryCapacity(id, v);
      if (this.pending[id] === v) {
        const rest = { ...this.pending };
        delete rest[id];
        this.pending = rest;
        this.save();
      }
    } catch { /* stays pending */ }
  }
  /** Send what is waiting, then take the account's values. */
  async refresh(server: BatteryServer): Promise<void> {
    for (const id of Object.keys(this.pending)) await this.push(server, id);
    const remote = validValues(await server.batteryCapacities());
    for (const [id, v] of Object.entries(this.pending)) {
      if (v === null) delete remote[id]; else remote[id] = v;
    }
    this.values = remote;
    this.save();
  }
}

export const batteryCapacity = new BatteryCapacityBook(typeof localStorage === "undefined" ? null : localStorage);
