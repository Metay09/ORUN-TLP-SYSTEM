// Charging and "full", worked out in the app from the collar's raw battery
// readings (owner decisions 2026-11-15/16: firmware sends facts, the app
// interprets). The charger (TP4054) charges to 4.20 V, stops below ~C/10 and
// starts again near 4.05 V; its status pin reaches only the LED. USB and the
// solar panel feed the same charger.
//
//  USB:  the reading is the charger's voltage. It climbs while charging; when
//        the charge ends it drops at once (bench: 4175 → 4148 mV in a minute)
//        and then sinks slowly. A climb, a peak and a drop while still on USB
//        = full.
//  Sun:  the panel comes and goes (clouds, shade, the animal lying on it), and
//        while it charges the reading is pushed up by the charge current. A
//        drop after a peak may just be a cloud, so "full" in the sun needs the
//        peak to reach this collar's own charge-end voltage learnt on USB.
//        While it charges, the percent shown is the last night's reading (no
//        sun, no charge current), not the inflated daytime voltage.
import { batteryPercent } from "./herd.ts";

export interface Reading { t: number; mv: number; usb?: boolean | null; power?: string | null }

export type ChargeState = "usb-full" | "usb-charging" | "usb" | "sun-full" | "sun-charging" | "battery";

export interface NightReading { t: number; mv: number; percent: number }

export interface Charge {
  state: ChargeState;
  /** This collar's charge-end voltage, learnt from a full charge on USB (null: not seen yet). */
  floatMv: number | null;
  /** The newest night reading (no sun, no USB) of the last ~30 hours. */
  night: NightReading | null;
  /** Last night against the night before, when the sun charged in between. */
  solar: { deltaPct: number; from: number; to: number } | null;
}

const HOUR = 3600_000;
export const RISE_MV = 12;          // a climb this big within RISE_WINDOW is charging, not noise
const RISE_WINDOW = HOUR;
export const DROP_MV = 15;          // the charge-end drop (bench 27 mV); firmware P2 used 15
export const FULL_MIN_MV = 4080;    // a USB peak below this is not a charge end
const NEAR_FLOAT_MV = 15;           // a sun peak this close to the learnt charge end counts as full
const NIGHT_FROM_H = 0, NIGHT_TO_H = 5;   // local hours with no sun in any season here

const onUsb = (r: Reading) => r.usb === true;
const isNight = (t: number) => { const h = new Date(t).getHours(); return h >= NIGHT_FROM_H && h < NIGHT_TO_H; };

/** The trailing readings taken on USB, oldest first (empty when the newest is not on USB). */
function usbRun(rs: Reading[]): Reading[] {
  let i = rs.length;
  while (i > 0 && onUsb(rs[i - 1])) i--;
  return rs.slice(i);
}

/** Peak of a full charge inside one run of readings, or null: rose, peaked ≥ min, then dropped. */
function chargeEnd(run: Reading[], minPeak: number): Reading | null {
  let peak: Reading | null = null, low = Infinity;
  for (const r of run) {
    if (peak && r.mv <= peak.mv - DROP_MV && peak.mv >= minPeak && low < peak.mv) return peak;
    if (!peak || r.mv > peak.mv) peak = r;
    low = Math.min(low, r.mv);
  }
  return null;
}

/** The charge-end voltage of the newest full charge on USB in these readings. */
export function learnFloat(rs: Reading[]): number | null {
  let found: number | null = null, run: Reading[] = [];
  const close = () => { const p = chargeEnd(run, FULL_MIN_MV); if (p) found = p.mv; run = []; };
  for (const r of rs) {
    if (onUsb(r)) run.push(r);
    else if (run.length) close();
  }
  if (run.length) close();
  return found;
}

/** Charging from the panel now: the newest reading off USB climbed within the last hour. */
function sunCharging(rs: Reading[]): boolean {
  const last = rs[rs.length - 1];
  if (!last || onUsb(last)) return false;
  const prev = rs[rs.length - 2];
  if (!prev || onUsb(prev) || last.mv < prev.mv) return false;
  let low = Infinity;
  for (let i = rs.length - 2; i >= 0 && rs[i].t >= last.t - RISE_WINDOW && !onUsb(rs[i]); i--) low = Math.min(low, rs[i].mv);
  return last.mv - low >= RISE_MV;
}

/** Charged by the panel up to this collar's own charge end, then stopped (last 3 hours). */
function sunFull(rs: Reading[], floatMv: number | null): boolean {
  const last = rs[rs.length - 1];
  if (floatMv === null || !last || onUsb(last)) return false;
  const run: Reading[] = [];
  for (let i = rs.length - 1; i >= 0 && rs[i].t >= last.t - 3 * HOUR && !onUsb(rs[i]); i--) run.unshift(rs[i]);
  return chargeEnd(run, floatMv - NEAR_FLOAT_MV) !== null;
}

/** For each night, its last reading off USB (closest to dawn), oldest first. */
export function nightReadings(rs: Reading[]): NightReading[] {
  const byDay = new Map<number, Reading>();
  for (const r of rs) {
    if (onUsb(r) || r.power === "NO_BATTERY" || r.mv <= 0 || !isNight(r.t)) continue;
    const d = new Date(r.t); d.setHours(0, 0, 0, 0);
    byDay.set(d.getTime(), r);
  }
  return [...byDay.values()].sort((a, b) => a.t - b.t).map((r) => ({ t: r.t, mv: r.mv, percent: batteryPercent(r.mv) ?? 0 }));
}

/** Last night against the night before, if the panel charged in between and USB did not. */
function solarBalance(rs: Reading[], nights: NightReading[]): Charge["solar"] {
  if (nights.length < 2) return null;
  const a = nights[nights.length - 2], b = nights[nights.length - 1];
  if (b.t - a.t > 30 * HOUR) return null;
  const between = rs.filter((r) => r.t > a.t && r.t < b.t);
  if (!between.length || between.some(onUsb)) return null;
  let charged = false;
  for (let i = 1; i <= between.length && !charged; i++) charged = sunCharging(between.slice(0, i));
  return charged ? { deltaPct: b.percent - a.percent, from: a.t, to: b.t } : null;
}

/** What the battery is doing, from readings sorted oldest first. */
export function chargeOf(readings: Reading[], now: number): Charge {
  const rs = readings.filter((r) => Number.isFinite(r.t) && Number.isFinite(r.mv));
  const floatMv = learnFloat(rs);
  const nights = nightReadings(rs);
  const lastNight = nights[nights.length - 1];
  const night = lastNight && now - lastNight.t <= 30 * HOUR ? lastNight : null;
  const solar = solarBalance(rs, nights);
  const usb = usbRun(rs);
  let state: ChargeState;
  if (usb.length) {
    if (chargeEnd(usb, FULL_MIN_MV)) state = "usb-full";
    else if (usb.length >= 2 && usb[usb.length - 1].mv - Math.min(...usb.map((r) => r.mv)) >= 8) state = "usb-charging";
    else state = "usb";
  } else if (sunFull(rs, floatMv)) state = "sun-full";
  else if (sunCharging(rs)) state = "sun-charging";
  else state = "battery";
  return { state, floatMv, night, solar };
}

/** Charging words for the battery card; null: on battery (the level speaks). */
export function chargeTitle(state: ChargeState): string | null {
  switch (state) {
    case "usb-full": return "USB'de · doldu";
    case "usb-charging": return "USB'de · şarj oluyor";
    case "usb": return "USB'de";
    case "sun-full": return "Güneşle doldu";
    case "sun-charging": return "Güneşte şarj oluyor";
    default: return null;
  }
}
