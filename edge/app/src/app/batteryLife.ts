// Pil ömrü (sahip isteği 2026-11-11): iki hesap yan yana, hangisinin neye
// dayandığı yazar; güneş katkısı katılmaz (9. adım).
//  - Beklenen: pil kapasitesi (mAh), rapor aralığı ve üreticilerin yayımladığı
//    akımlardan. Bugünkü tasma yazılımında işlemci gerçekten uyumuyor (Y3);
//    taban akım ona göre ayrı verilir.
//  - Ölçülen: sahada pilin günlük düşüşünden (batteryOutlook). Pil düşmüyorsa
//    (USB'de ya da güneşle dengede) ölçülemez.
// mAh her tasma için ayrı girilir (sahip seçimi); bu telefonda saklanır.

/** Published figures (mA). Each value says where it comes from. */
export const PUBLISHED = {
  /** RAK4631 + WisBlock Base, Arduino loop without real sleep: ~2.2 mA measured by RAK forum users. */
  baseAwakeMa: 2.2,
  /** RAK4631 on a WisBlock Base in real sleep: ~24 µA measured (RAK forum), plus the accelerometer. */
  baseSleepMa: 0.03,
  /** u-blox ZOE-M8Q (RAK12500), continuous mode at 3 V: 25 mA (u-blox product summary). */
  gnssMa: 25,
  /** u-blox ZOE-M8 cold start ~26 s; the receiver's rail is cut between reports. */
  gnssOnS: 30,
  /** Semtech SX1262 TX at +14 dBm (the ORUN default): 45 mA; RX: 4.6 mA (Semtech). */
  txMa: 45,
  rxMa: 4.6,
  /** ORUN radio_config: listen 10 s after each report. */
  rxAfterReportS: 10,
};
/** A LiPo is not run to 0 %; count 80 % of the label as usable. */
export const USABLE = 0.8;
/** Below this interval the GNSS stays on (gnss_config kShortIntervalThreshold). */
const SHORT_INTERVAL_S = 60;

/** LoRa time on air, seconds (Semtech formula; explicit header, CRC on, LDRO for SF11/12 at 125 kHz). */
export function timeOnAirS(payloadBytes: number, sf = 11, bwHz = 125_000, cr = 1, preamble = 8): number {
  const tsym = 2 ** sf / bwHz;
  const de = sf >= 11 && bwHz <= 125_000 ? 1 : 0;
  const n = 8 + Math.max(Math.ceil((8 * payloadBytes - 4 * sf + 28 + 16) / (4 * (sf - 2 * de))) * (cr + 4), 0);
  return (preamble + 4.25) * tsym + n * tsym;
}

export interface Expected { avgMa: number; mAhPerDay: number; days: number; }

/** Average current and days on a full battery for one report every `intervalS`. */
export function expectedLife(mAh: number, intervalS: number, sleep: boolean, gnssOnS: number = PUBLISHED.gnssOnS): Expected {
  const P = PUBLISHED;
  const base = sleep ? P.baseSleepMa : P.baseAwakeMa;
  const gnss = intervalS < SHORT_INTERVAL_S ? P.gnssMa : P.gnssMa * Math.min(1, gnssOnS / intervalS);
  const radio = (P.txMa * timeOnAirS(100) + P.rxMa * P.rxAfterReportS) / intervalS;
  const avgMa = base + gnss + radio;
  const mAhPerDay = avgMa * 24;
  return { avgMa, mAhPerDay, days: (mAh * USABLE) / mAhPerDay };
}

export interface Measured { mAhPerDay: number; avgMa: number; daysFull: number; daysLeft: number | null; }

/** From the measured daily drain (% per day, negative = draining). Null when the battery does not drain. */
export function measuredLife(mAh: number, perDayPct: number | null, nowPct: number | null): Measured | null {
  if (perDayPct === null || perDayPct > -0.5) return null;
  const mAhPerDay = (mAh * -perDayPct) / 100;
  return {
    mAhPerDay, avgMa: mAhPerDay / 24,
    daysFull: (mAh * USABLE) / mAhPerDay,
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
