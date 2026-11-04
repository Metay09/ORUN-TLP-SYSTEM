// 4a: tasmanın canlı durumu. USB'de "APP ...?" satırları, Bluetooth'ta ORUN
// uygulama servisi aynı şeyi söyler; ikisi de önce aynı anahtar=değer
// biçimine çevrilir (USB satırlarının adları), sonra tek yerde okunur.
import { checkAnswer, type AppMessage, type Family } from "./bleAppFrames.ts";
import type { LineConsole } from "./addTracker.ts";

export type Raw = Partial<Record<Family, Record<string, string>>>;
export type Via = "usb" | "ble";
export type SvcState = "on" | "off" | "blocked" | "degraded";

export interface TrackerStatus {
  via: Via;
  at: number;                      // ms, when it was read
  role: "tracker" | "relay" | "gateway" | null;
  firmware: string | null;
  uptimeMs: number | null;
  interval: { requestedS: number; effectiveS: number | null; stored: boolean } | null;
  /** 4c-1a: Bluetooth open after each report (s, 0 = off); null when the firmware does not say. */
  bleWindowS?: number | null;
  gnss: { state: number; detected: boolean; fixes: number; attempts: number; timeouts: number; lastTtffMs: number } | null;
  battery: { valid: boolean; mv: number; percent: number | null; state: "unknown" | "normal" | "low" | "critical"; calibrated: boolean } | null;
  services: {
    explicit: boolean;
    /** what was asked for (null in AUTO: the firmware decides from the hardware) */
    requested: { tracking: boolean; relay: boolean; receive: boolean } | null;
    tracking: SvcState; trackingWhy: string | null; relay: SvcState; receive: boolean;
  } | null;
  fence: { state: "unavailable" | "clear" | "configured"; areas: number; vertices: number; position: "unknown" | "inside" | "outside" } | null;
  storage: { ready: boolean; count: number; capacity: number; overwritten: number } | null;
  missing: Family[];               // asked, no answer
}

export const FAMILIES: Family[] = ["device", "config", "tracking", "power", "services", "geofence", "storage"];

// ---------------------------------------------------------------- USB ---
/** "APP X k=v k=v" → {k: v}. */
export function keyValues(line: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const part of line.split(/\s+/)) {
    const i = part.indexOf("=");
    if (i > 0) out[part.slice(0, i)] = part.slice(i + 1);
  }
  return out;
}

const USB: Record<Family, { ask: string; answer: RegExp }> = {
  config: { ask: "APP CONFIG?", answer: /^APP RESULT id=\d+ code=OK config_backend_ready=/ },
  device: { ask: "APP DEVICE?", answer: /^APP DEVICE id=/ },
  tracking: { ask: "APP TRACKING?", answer: /^APP TRACKING id=/ },
  geofence: { ask: "APP GEOFENCE?", answer: /^APP GEOFENCE id=/ },
  storage: { ask: "APP STORAGE?", answer: /^APP STORAGE id=/ },
  power: { ask: "APP POWER?", answer: /^APP POWER id=/ },
  services: { ask: "APP SERVICES?", answer: /^APP SERVICES mode=/ },
};
const USB_FAIL = /^(APP RESULT id=\d+ code=(?!OK)\w+|APP BUSY|APP REJECTED)/;

/** Ask every family over USB; a family that does not answer is left out. */
export async function readRawUsb(con: LineConsole, families: Family[] = FAMILIES): Promise<Raw> {
  const raw: Raw = {};
  for (const f of families) {
    await con.send(USB[f].ask);
    const line = await con.waitFor((l) => (USB[f].answer.test(l) || USB_FAIL.test(l) ? l : null), 3000);
    if (line && USB[f].answer.test(line)) raw[f] = keyValues(line);
  }
  return raw;
}

// ---------------------------------------------------------------- BLE ---
const le16 = (p: Uint8Array, o: number) => p[o] | (p[o + 1] << 8);
const le32 = (p: Uint8Array, o: number) => (p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)) >>> 0;
const yes = (b: number) => (b ? "yes" : "no");
const s = (n: number) => String(n);

/** The same names USB prints, from the firmware's serviceStateName(). */
export function serviceStateName(state: number, reason: number): string {
  if (state === 0) return "OFF";
  if (state === 1) return "ON";
  if (state === 3) return "DEGRADED";
  return ({ 5: "BLOCKED_GNSS_ABSENT", 4: "BLOCKED_GNSS_PENDING", 7: "BLOCKED_GNSS_FAULT",
            8: "BLOCKED_GNSS_UNAVAILABLE", 3: "BLOCKED_UNSUPPORTED", 2: "BLOCKED_INVALID" } as Record<number, string>)[reason] ?? "BLOCKED";
}
const BATTERY = ["UNKNOWN", "NORMAL", "LOW", "CRITICAL"];

function servicesLetters(bits: number): string {
  const t = (bits & 1 ? "T" : "") + (bits & 2 ? "R" : "") + (bits & 4 ? "A" : "");
  return t || "NONE";
}

/** One Bluetooth answer → the USB key names. */
export function decodeBle(family: Family, m: AppMessage): Record<string, string> {
  const p = checkAnswer(m, family);
  const need = { config: 10, device: 36, tracking: 36, geofence: 9, storage: 40, power: 16, services: 9 }[family];
  if (p.length < need) throw new Error("Cihazın yanıtı kısa.");
  switch (family) {
    case "config":
      return { config_backend_ready: yes(p[1] & 1), source: p[1] & 2 ? "stored" : "default",
               tracking_interval_seconds: s(le32(p, 2)), battery_capacity_mah: s(le32(p, 6)),
               // 4c-1a: appended; older firmware answers 10 bytes.
               ...(p.length >= 12 ? { ble_window_seconds: s(le16(p, 10)) } : {}) };
    case "device": {
      const fw = new TextDecoder().decode(p.subarray(20, 36)).replace(/\0.*$/s, "");
      return { fw, rev: s(p[1]), role: s(p[2]), role_mode: p[3] & 1 ? "AUTO" : "OVERRIDE", watchdog: yes(p[3] & 2),
               relay_applied: yes(p[3] & 4), gnss_presence: s(p[4]), gnss_health: s(p[5]),
               accel_presence: s(p[6]), accel_health: s(p[7]), tracking_state: s(p[8]), tracking_reason: s(p[9]),
               relay_state: s(p[10]), relay_reason: s(p[11]), uptime_ms_mod32: s(le32(p, 12)),
               reset: "0x" + le32(p, 16).toString(16).toUpperCase().padStart(8, "0") };
    }
    case "tracking":
      return { requested_s: s(le32(p, 4)), applied_s: s(le32(p, 8)), effective_s: s(le32(p, 12)),
               cadence: s(p[2]), gnss_state: s(p[3]), detected: yes(p[1] & 4), extra_fix: yes(p[1] & 8),
               attempts: s(le32(p, 16)), fresh: s(le32(p, 20)), timeouts: s(le32(p, 24)), invalid: s(le32(p, 28)),
               last_ttff_ms: s(le32(p, 32)), config_ready: yes(p[1] & 1), source: p[1] & 2 ? "stored" : "default" };
    case "geofence":
      return { resource: s(p[1]), token: s(p[2]), areas: s(p[4]), vertices: s(le16(p, 5)),
               runtime: p[3] & 1 ? "configured" : "clear", confirmed: yes(p[3] & 2), state: s(p[7]),
               confirmation: p[3] & 4 ? "active" : "idle", cadence: s(p[8]) };
    case "storage":
      return { history_ready: yes(p[1] & 1), busy: yes(p[1] & 2), count: s(le32(p, 4)), capacity: s(le32(p, 8)),
               overwritten: s(le32(p, 12)), append_failures: s(le32(p, 16)) };
    case "power":
      return { battery: p[1] & 1 ? "MEASURED" : "NONE", state: BATTERY[p[2]] ?? "UNKNOWN",
               percent_est: p[1] & 1 && p[3] <= 100 ? s(p[3]) : "-", mv: s(le16(p, 4)), raw: s(le16(p, 6)),
               age_ms: s(le32(p, 8)), readings: s(le32(p, 12)), calibrated: yes(p[1] & 2) };
    case "services": {
      const explicit = p[1] === 1;
      return { mode: explicit ? "EXPLICIT" : "AUTO", requested: explicit ? servicesLetters(p[2]) : "-",
               tracking: serviceStateName(p[4], p[5]), relay: serviceStateName(p[6], p[7]),
               receive: p[3] & 2 ? "ON" : "OFF" };
    }
  }
}

// ---------------------------------------------------------- one model ---
const num = (v: string | undefined): number | null => (v !== undefined && /^\d+$/.test(v) ? Number(v) : null);

function svc(v: string | undefined): { state: SvcState; why: string | null } {
  if (v === "ON") return { state: "on", why: null };
  if (v === "DEGRADED") return { state: "degraded", why: null };
  if (v?.startsWith("BLOCKED")) return { state: "blocked", why: v };
  return { state: "off", why: null };
}

export function buildStatus(raw: Raw, via: Via, at: number): TrackerStatus {
  const d = raw.device, c = raw.config, t = raw.tracking, p = raw.power, sv = raw.services, g = raw.geofence, st = raw.storage;
  const role = d ? ({ "0": "tracker", "1": "relay", "2": "gateway" } as const)[d.role as "0" | "1" | "2"] ?? null : null;
  const requested = num(t?.requested_s) ?? num(c?.tracking_interval_seconds);
  const tracking = svc(sv?.tracking);
  return {
    via, at, role,
    firmware: d?.fw || null,
    uptimeMs: num(d?.uptime_ms_mod32),
    interval: requested === null ? null : {
      requestedS: requested, effectiveS: num(t?.effective_s),
      stored: (t?.source ?? c?.source) === "stored",
    },
    bleWindowS: num(c?.ble_window_seconds),
    gnss: t ? { state: num(t.gnss_state) ?? 0, detected: t.detected === "yes", fixes: num(t.fresh) ?? 0,
                attempts: num(t.attempts) ?? 0, timeouts: num(t.timeouts) ?? 0, lastTtffMs: num(t.last_ttff_ms) ?? 0 } : null,
    battery: p ? { valid: p.battery === "MEASURED", mv: num(p.mv) ?? 0, percent: num(p.percent_est),
                   state: (["normal", "low", "critical"].includes(p.state?.toLowerCase() ?? "") ? p.state.toLowerCase() : "unknown") as "unknown" | "normal" | "low" | "critical", calibrated: p.calibrated === "yes" } : null,
    services: sv ? { explicit: sv.mode === "EXPLICIT",
                     requested: sv.mode === "EXPLICIT" && sv.requested && sv.requested !== "-"
                       ? { tracking: sv.requested.includes("T"), relay: sv.requested.includes("R"), receive: sv.requested.includes("A") }
                       : null,
                     tracking: tracking.state, trackingWhy: tracking.why,
                     relay: svc(sv.relay).state, receive: sv.receive === "ON" } : null,
    fence: g ? { state: g.runtime === "configured" ? "configured" : g.resource === "0" ? "unavailable" : "clear",
                 areas: num(g.areas) ?? 0, vertices: num(g.vertices) ?? 0,
                 position: g.confirmed === "yes" ? (g.state === "1" ? "inside" : g.state === "2" ? "outside" : "unknown") : "unknown" } : null,
    storage: st ? { ready: st.history_ready === "yes", count: num(st.count) ?? 0, capacity: num(st.capacity) ?? 0,
                    overwritten: num(st.overwritten) ?? 0 } : null,
    missing: FAMILIES.filter((f) => !raw[f]),
  };
}

// ------------------------------------------------------------- words ---
const GNSS = ["Takılı değil", "Aranıyor", "Kapalı (bekliyor)", "Açılıyor", "Aranıyor", "Bekliyor", "Başlıyor",
              "Konum arıyor", "Konum var", "Zaman aşımı", "Arıza", "Uyuyor"];
export const gnssWord = (n: number) => GNSS[n] ?? "Bilinmiyor";
export const svcWord = (v: SvcState, why: string | null = null) =>
  v === "on" ? "Açık" : v === "off" ? "Kapalı" : v === "degraded" ? "Kısıtlı"
    : why === "BLOCKED_GNSS_ABSENT" ? "Açık değil: GPS takılı değil"
    : why === "BLOCKED_GNSS_PENDING" ? "GPS bekleniyor" : "Çalışamıyor";
export const batteryWord = { unknown: "", normal: "", low: "düşük", critical: "çok düşük" } as const;

