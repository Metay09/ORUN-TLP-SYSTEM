// 4b: cihaz ayarları (sahip kararı 2026-11-02). Her ORUN cihazının ayarı
// hesapta durur: "istenen" (kullanıcının verdiği) ve "cihazda" (cihazdan en son
// okunan). İkisi farklıysa ayar bekliyor; cihaza bağlanınca sormadan yazılır.
// Ayar listesi bütün cihaz türleri için tektir; bir cihaz yalnız kendinde olan
// ayarları gösterir (sensör/vana gelince buraya yeni ayarlar eklenir).
import { fmt } from "./i18n/tr.ts";
import type { TrackerStatus } from "./trackerStatus.ts";
import { BLE_WINDOW, BLE_WINDOW_CHOICES, INTERVAL, INTERVAL_CHOICES } from "./trackerSettings.ts";

export interface SettingValues { interval_s?: number; tracking?: boolean; relay?: boolean; ble_window_s?: number; }
export type SettingKey = keyof SettingValues;
export type Via = "usb" | "ble" | "lora";

export interface DeviceSettingsEntry {
  wanted: SettingValues;
  wantedAt: number | null;          // ms
  reported: SettingValues | null;
  reportedAt: number | null;        // ms
  reportedVia: Via | null;
}

export interface SettingDef {
  key: SettingKey;
  title: string;
  help: string;
  kind: "choice" | "flag";
  choices?: number[];
  word(v: number | boolean): string;
}

export const SETTINGS: SettingDef[] = [
  { key: "interval_s", title: "Rapor aralığı", kind: "choice", choices: INTERVAL_CHOICES,
    help: "Cihaz kaç dakikada bir rapor (konum, hareket, pil) gönderir. Kısa aralık daha çok pil harcar.",
    word: (v) => fmt.period(v as number) },
  { key: "tracking", title: "Konum takibi", kind: "flag",
    help: "Kapalıyken cihaz GPS'i açmaz, konum göndermez.",
    word: (v) => (v ? "Açık" : "Kapalı") },
  { key: "relay", title: "Aktarma", kind: "flag",
    help: "Başka cihazların kayıtlarını gateway'e iletir. Açıkken telsiz hep dinler, pil daha çok gider.",
    word: (v) => (v ? "Açık" : "Kapalı") },
  { key: "ble_window_s", title: "Rapordan sonra Bluetooth", kind: "choice", choices: BLE_WINDOW_CHOICES,
    help: "Her rapordan sonra Bluetooth bu kadar açık kalır; telefon bu sürede bağlanabilir, bağlanınca iş bitene kadar açık kalır. Kapalıyken yalnız açılışta 10 dakika açılır. Uzun süre biraz daha pil harcar.",
    word: (v) => bleWindowWord(v as number) },
];

export function bleWindowWord(s: number): string {
  if (s === 0) return "Kapalı";
  return s < 60 ? `${s} sn` : `${s / 60} dk`;
}

export function validValue(key: SettingKey, v: unknown): boolean {
  if (key === "interval_s") return Number.isInteger(v) && (v as number) >= INTERVAL.minS && (v as number) <= INTERVAL.maxS;
  if (key === "ble_window_s") return Number.isInteger(v) && (v as number) >= 0 && (v as number) <= BLE_WINDOW.maxS;
  return typeof v === "boolean";
}

function cleanValues(v: unknown): SettingValues {
  const out: SettingValues = {};
  if (!v || typeof v !== "object") return out;
  for (const def of SETTINGS) {
    const x = (v as Record<string, unknown>)[def.key];
    if (validValue(def.key, x)) (out as Record<string, unknown>)[def.key] = x;
  }
  return out;
}

/** Keys the user asked for that the device does not have (or that were never read). */
export function pendingKeys(e: DeviceSettingsEntry | null | undefined): SettingKey[] {
  if (!e) return [];
  return (Object.keys(e.wanted) as SettingKey[]).filter((k) => e.reported?.[k] !== e.wanted[k]);
}

/** What a live read says the tracker has (only what it actually reported). */
export function valuesFromStatus(st: TrackerStatus): SettingValues {
  const out: SettingValues = {};
  if (st.interval) out.interval_s = st.interval.requestedS;
  if (typeof st.bleWindowS === "number") out.ble_window_s = st.bleWindowS;
  if (st.services?.requested) { out.tracking = st.services.requested.tracking; out.relay = st.services.requested.relay; }
  return out;
}

export type SettingState = "device" | "waiting" | "observed" | "unknown";
export interface SettingRow { def: SettingDef; value: number | boolean | null; onDevice: number | boolean | null; state: SettingState; }

/**
 * `observed`: what the device's own records show when it was never read
 * directly (e.g. the report period of its records); shown, never sent.
 */
export function settingRows(e: DeviceSettingsEntry | null | undefined, observed: SettingValues = {}): SettingRow[] {
  return SETTINGS.map((def) => {
    const wanted = e?.wanted[def.key], has = e?.reported?.[def.key], seen = observed[def.key];
    if (wanted !== undefined && wanted !== has) return { def, value: wanted, onDevice: has ?? seen ?? null, state: "waiting" };
    if (has !== undefined) return { def, value: has, onDevice: has, state: "device" };
    if (seen !== undefined) return { def, value: seen, onDevice: seen, state: "observed" };
    return { def, value: null, onDevice: null, state: "unknown" };
  });
}

/** One setting across the whole herd (Ayarlar → Tasma ayarları). */
export interface FleetRow {
  def: SettingDef;
  total: number;
  values: { value: number | boolean; count: number }[];   // most common first
  waiting: number;                                         // asked for, not yet on the collar
  unknown: number;                                         // never read, nothing observed
}

export function fleetRows(
  devices: { id: string; observed?: SettingValues }[], entries: Record<string, DeviceSettingsEntry>,
): FleetRow[] {
  const perDevice = devices.map((d) => settingRows(entries[d.id.toUpperCase()], d.observed));
  return SETTINGS.map((def, i) => {
    const counts = new Map<number | boolean, number>();
    let waiting = 0, unknown = 0;
    for (const rows of perDevice) {
      const r = rows[i];
      if (r.state === "waiting") waiting++;
      if (r.value === null) unknown++;
      else counts.set(r.value, (counts.get(r.value) ?? 0) + 1);
    }
    const values = [...counts].map(([value, count]) => ({ value, count })).sort((x, y) => y.count - x.count);
    return { def, total: devices.length, values, waiting, unknown };
  });
}

/** The value every collar shares, or null when they differ / none is known. */
export const fleetCommon = (r: FleetRow): number | boolean | null =>
  (r.values.length === 1 && r.unknown === 0 ? r.values[0].value : null);

export function fleetLine(r: FleetRow): { value: string; sub: string; waiting: boolean } {
  const value = r.values.length === 0 ? "—" : r.values.length === 1 ? r.def.word(r.values[0].value) : "Karışık";
  const parts: string[] = [];
  if (r.values.length > 1) parts.push(r.values.map((v) => `${v.count} tasma ${r.def.word(v.value)}`).join(" · "));
  if (r.waiting) parts.push(`${r.waiting} tasma bekliyor, bağlanınca işlenir`);
  else if (r.values.length === 1 && r.unknown === 0) parts.push(r.total === 1 ? "Tasmada" : `${r.total} tasmanın hepsinde`);
  if (r.unknown) parts.push(`${r.unknown} tasmadan henüz okunmadı`);
  return { value, sub: parts.join(" · "), waiting: r.waiting > 0 };
}

export interface SettingsServer {
  deviceSettings(): Promise<Record<string, DeviceSettingsEntry>>;
  wantSettings(devices: string[], set: SettingValues): Promise<void>;
  reportSettings(device: string, values: SettingValues, via: Via): Promise<void>;
}

type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.deviceSettings.v1";
const ID16 = /^[0-9A-F]{16}$/;
type Pending = { kind: "want"; devices: string[]; set: SettingValues } | { kind: "report"; device: string; values: SettingValues; via: Via };

function cleanEntries(v: unknown): Record<string, DeviceSettingsEntry> {
  const out: Record<string, DeviceSettingsEntry> = {};
  if (!v || typeof v !== "object") return out;
  for (const [k, e] of Object.entries(v as Record<string, Record<string, unknown>>)) {
    if (!ID16.test(k) || !e || typeof e !== "object") continue;
    out[k] = {
      wanted: cleanValues(e.wanted), wantedAt: typeof e.wantedAt === "number" ? e.wantedAt : null,
      reported: e.reported ? cleanValues(e.reported) : null,
      reportedAt: typeof e.reportedAt === "number" ? e.reportedAt : null,
      reportedVia: e.reportedVia === "usb" || e.reportedVia === "ble" || e.reportedVia === "lora" ? e.reportedVia : null,
    };
  }
  return out;
}

const blank = (): DeviceSettingsEntry => ({ wanted: {}, wantedAt: null, reported: null, reportedAt: null, reportedVia: null });

/** Phone copy of the account's device settings; changes made offline wait here. */
export class DeviceSettingsBook {
  private entries: Record<string, DeviceSettingsEntry> = {};
  private queue: Pending[] = [];
  private listeners = new Set<() => void>();
  private store: Store | null;

  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      const v = raw ? JSON.parse(raw) as { entries?: unknown; queue?: unknown } : null;
      this.entries = cleanEntries(v?.entries);
      this.queue = Array.isArray(v?.queue) ? (v!.queue as Pending[]).filter((p) => p && (p.kind === "want" || p.kind === "report")) : [];
    } catch { /* start empty */ }
  }

  private save(): void {
    try { this.store?.setItem(KEY, JSON.stringify({ entries: this.entries, queue: this.queue })); } catch { /* fine */ }
    for (const fn of this.listeners) fn();
  }

  subscribe(fn: () => void): () => void { this.listeners.add(fn); return () => { this.listeners.delete(fn); }; }
  snapshot(): Record<string, DeviceSettingsEntry> { return this.entries; }
  get(device: string): DeviceSettingsEntry | null { return this.entries[device.toUpperCase()] ?? null; }
  waiting(): number { return this.queue.length; }

  /** The user's choice for one or many devices. */
  async want(devices: string[], set: SettingValues, server: SettingsServer | null, now = Date.now()): Promise<void> {
    const ids = [...new Set(devices.map((d) => d.toUpperCase()))].filter((d) => ID16.test(d));
    const clean = cleanValues(set);
    if (!ids.length || !Object.keys(clean).length) throw new Error("Ayar ya da cihaz seçilmedi.");
    const next = { ...this.entries };
    for (const d of ids) {
      const e = next[d] ?? blank();
      next[d] = { ...e, wanted: { ...e.wanted, ...clean }, wantedAt: now };
    }
    this.entries = next;
    this.queue = [...this.queue, { kind: "want", devices: ids, set: clean }];
    this.save();
    if (server) await this.flush(server);
  }

  /** What the device itself said it has (after a live read or a write). */
  async report(device: string, values: SettingValues, via: Via, server: SettingsServer | null, now = Date.now()): Promise<void> {
    const d = device.toUpperCase();
    const clean = cleanValues(values);
    if (!ID16.test(d) || !Object.keys(clean).length) return;
    const e = this.entries[d] ?? blank();
    this.entries = { ...this.entries, [d]: { ...e, reported: { ...(e.reported ?? {}), ...clean }, reportedAt: now, reportedVia: via } };
    this.queue = [...this.queue, { kind: "report", device: d, values: clean, via }];
    this.save();
    if (server) await this.flush(server);
  }

  private async flush(server: SettingsServer): Promise<void> {
    while (this.queue.length) {
      const p = this.queue[0];
      try {
        if (p.kind === "want") await server.wantSettings(p.devices, p.set);
        else await server.reportSettings(p.device, p.values, p.via);
      } catch { return; }   // stays queued; sent on the next refresh
      this.queue = this.queue.slice(1);
      this.save();
    }
  }

  async refresh(server: SettingsServer): Promise<void> {
    await this.flush(server);
    if (this.queue.length) return;          // the account would overwrite unsent changes
    this.entries = cleanEntries(Object.fromEntries(Object.entries(await server.deviceSettings())));
    this.save();
  }
}
