// "Tanıtım sürüsü": a sample herd for showing the app without devices. Built
// from the same record shape the phone ledger gives (core Point) so every
// screen runs the real code path; the UI marks it clearly as a sample.
//
// Deterministic and stateless in time: a record's position is a function of
// its measurement time, so history stays stable while new records "arrive"
// every report period. The story: a herd moving barn -> slope pasture ->
// water -> shade -> stony pasture -> barn each day; one collar on low battery,
// one nearly empty, one cow quiet behind the ridge, one gone far (no news for
// hours), one whose collar missed fixes one night, one cow still for hours
// (calving) this morning.
import type { Point, PointsResult } from "../core/points.ts";
import type { Snapshot, JournalEntry } from "./controller.ts";
import type { EdgeSource } from "./source.ts";
import type { AssetBook } from "./stores.ts";
import { blankProfile } from "./stores.ts";
import type { Zone } from "./zones.ts";

const T = 900;                     // report period, s
const HOUR = 3600_000;
const DAY = 24 * HOUR;
const DAYS = 7;
const CENTER = { lat: 37.6107, lon: 28.0546 };
const KX = 111_320 * Math.cos((CENTER.lat * Math.PI) / 180), KY = 111_320;
export const DEMO_STATION = { id: "09A462BD4B275BA5", x: 60, y: 140 };

const toLL = (x: number, y: number) => ({ lat: CENTER.lat + y / KY, lon: CENTER.lon + x / KX });
export const DEMO_STATION_POS = toLL(DEMO_STATION.x, DEMO_STATION.y);

// Deterministic noise in [0, 1).
function hash(a: number, b: number): number {
  let h = (a * 374761393 + b * 668265263) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177) >>> 0;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

type Way = [number, number];
const BARN: Way = [0, 0], SLOPE: Way = [360, 430], WATER: Way = [-240, 190], SHADE: Way = [-120, 320], STONY: Way = [-430, -270];
// hour of day -> herd centre and what the herd does (activity share).
const PLAN: [number, Way, number][] = [
  [0, BARN, 0.03], [6, BARN, 0.08], [7, SLOPE, 0.8], [11, SLOPE, 0.55], [12, WATER, 0.75],
  [12.5, SHADE, 0.1], [14, SHADE, 0.08], [15, STONY, 0.8], [18.5, STONY, 0.55], [19.5, BARN, 0.7], [24, BARN, 0.03],
];

function herdAt(hour: number): { x: number; y: number; act: number; spread: number } {
  for (let i = 1; i < PLAN.length; i++) {
    const [h1, w1, a1] = PLAN[i], [h0, w0] = PLAN[i - 1];
    if (hour <= h1) {
      const f = (hour - h0) / (h1 - h0);
      const moving = w0 !== w1;
      return { x: w0[0] + (w1[0] - w0[0]) * f, y: w0[1] + (w1[1] - w0[1]) * f, act: a1, spread: moving ? 40 : w1 === BARN ? 22 : 120 };
    }
  }
  return { x: 0, y: 0, act: 0.03, spread: 22 };
}

function terrain(x: number, y: number): number {
  // Ellipsoid height (the collar reports WGS84 height): ~36 m above sea level here.
  return 36 + 410 + 75 * Math.exp(-((x - 360) ** 2 + (y - 430) ** 2) / (2 * 320 ** 2)) +
    55 * Math.exp(-((x - 700) ** 2 + (y - 150) ** 2) / (2 * 160 ** 2)) - 25 * Math.exp(-((x + 300) ** 2 + (y + 200) ** 2) / (2 * 380 ** 2));
}

// Radio: distance loss from the station plus a ridge east of x = 560 m.
function rssiAt(x: number, y: number): number {
  const d = Math.hypot(x - DEMO_STATION.x, y - DEMO_STATION.y);
  const ridge = x > 560 ? -22 - Math.min(20, (x - 560) / 15) : 0;
  return -62 - 26 * Math.log10(Math.max(d, 20) / 20) + ridge;
}
const HEARD = -127;
const snrOf = (rssi: number) => Math.max(-18, Math.min(11, (rssi + 116) * 0.6));

interface Cow {
  id: string; name: string; tag: string; breed: string; sex: "female" | "male"; birth: string; color: string;
  offset: number; base: number; notes?: [number, string][];
  // Story windows, in hours before the moment the sample was opened (anchor).
  trips?: { from: number; back: number | null; at: [number, number] }[];  // left the herd for `at`
  missed?: [number, number];                          // no fixes (collar missed them)
  still?: [number, number];                           // no movement
}

const COWS: Cow[] = [
  { id: "0E8ADE7E71531AA3", name: "Sarıkız", tag: "TR 09 4471 2093", breed: "Simental", sex: "female", birth: "2021-03-14", color: "#2E8B57", offset: 0, base: 4020, notes: [[52, "Şap aşısı yapıldı"]] },
  { id: "0E8ADE7E71530B21", name: "Karabaş", tag: "TR 09 4471 2107", breed: "Yerli Kara", sex: "male", birth: "2020-05-02", color: "#B4475C", offset: 170, base: 3600 },
  { id: "0E8ADE7E71537C40", name: "Benekli", tag: "TR 09 4471 2111", breed: "Holştayn", sex: "female", birth: "2022-01-20", color: "#A57A1E", offset: 320, base: 3990,
    trips: [{ from: 27, back: 25.5, at: [700, 160] }, { from: 2.4, back: null, at: [690, 120] }] },
  { id: "0E8ADE7E7153D901", name: "Duman", tag: "TR 09 4471 2128", breed: "Boz ırk", sex: "female", birth: "2019-04-11", color: "#6D5BD0", offset: 455, base: 3960, trips: [{ from: 5.3, back: null, at: [980, 940] }] },
  { id: "0E8ADE7E71534E12", name: "Akkız", tag: "TR 09 4471 2132", breed: "Simental", sex: "female", birth: "2021-09-08", color: "#3D7EA6", offset: 610, base: 4040, missed: [34, 31.8] },
  { id: "0E8ADE7E715360A7", name: "Yıldız", tag: "TR 09 4471 2146", breed: "Montofon", sex: "female", birth: "2022-06-30", color: "#C2611F", offset: 740, base: 4000 },
  { id: "0E8ADE7E71532F65", name: "Cesur", tag: "TR 09 4471 2150", breed: "Yerli Kara", sex: "male", birth: "2023-02-17", color: "#5B6B2E", offset: 95, base: 4060 },
  { id: "0E8ADE7E71538B3C", name: "Pamuk", tag: "TR 09 4471 2163", breed: "Holştayn", sex: "female", birth: "2020-11-25", color: "#5E8F8A", offset: 260, base: 3530 },
  { id: "0E8ADE7E71531D58", name: "Nazlı", tag: "TR 09 4471 2177", breed: "Simental", sex: "female", birth: "2019-12-03", color: "#8A4F9A", offset: 385, base: 4030, still: [9.5, 6], notes: [[30, "Doğum yaklaştı, gözle"], [5.5, "Buzağıladı · dişi buzağı, sağlıklı"]] },
  { id: "0E8ADE7E7153A642", name: "Boncuk", tag: "TR 09 4471 2181", breed: "Montofon", sex: "female", birth: "2023-04-09", color: "#2F6F8F", offset: 520, base: 4010 },
  { id: "0E8ADE7E71537711", name: "Maviş", tag: "TR 09 4471 2194", breed: "Boz ırk", sex: "female", birth: "2021-07-21", color: "#3D7EA6", offset: 660, base: 3980 },
  { id: "0E8ADE7E7153C0DE", name: "Ceylan", tag: "TR 09 4471 2208", breed: "Simental", sex: "female", birth: "2022-10-12", color: "#B4475C", offset: 805, base: 4050 },
];

// Yıldız: its newest record is ~45 min old (just aging, nothing wrong).
const LATE: Record<string, number> = { "0E8ADE7E715360A7": 45 * 60_000 };

const k15 = (t: number) => Math.floor(t / 900_000);
const iso = (ms: number) => new Date(ms).toISOString().replace(/\.\d{3}Z$/, "Z");

function positionOf(c: Cow, idx: number, t: number, anchor: number): { x: number; y: number; act: number } {
  const d = new Date(t);
  const hour = d.getHours() + d.getMinutes() / 60;
  const h = herdAt(hour);
  const ph = idx * 1.7, slow = t / (2.3 * HOUR);
  const r = h.spread * (0.45 + 0.55 * hash(idx, 7));
  let x = h.x + r * Math.cos(ph + slow) + (hash(idx, Math.floor(t / 60000)) - 0.5) * 8;
  let y = h.y + r * Math.sin(ph + slow * 0.8) + (hash(Math.floor(t / 60000), idx) - 0.5) * 8;
  let act = Math.max(0, Math.min(1, h.act + (hash(idx * 31, Math.floor(t / 900000)) - 0.5) * 0.25));
  for (const trip of c.trips ?? []) {
    // One hour to walk there; back with the herd after `back` (null: still away).
    const go = anchor - trip.from * HOUR, ret = trip.back === null ? Infinity : anchor - trip.back * HOUR;
    if (t <= go - HOUR || t >= ret + HOUR) continue;
    const f = Math.min(1, (t - (go - HOUR)) / HOUR, (ret + HOUR - t) / HOUR);
    x += (trip.at[0] - x) * f;
    y += (trip.at[1] - y) * f;
    act = 0.6;
  }
  if (c.still && t >= anchor - c.still[0] * HOUR && t <= anchor - c.still[1] * HOUR) {
    // Lying down (calving): stays where the stillness began.
    const s0 = anchor - c.still[0] * HOUR;
    if (t > s0 + 60_000) { const p0 = positionOf({ ...c, still: undefined }, idx, s0, anchor); x = p0.x + (hash(idx, k15(t)) - 0.5) * 3; y = p0.y; }
    act = 0;
  }
  return { x, y, act };
}

/** Records measured up to `now`; the story is told relative to `anchor`. */
export function demoPoints(now: number, anchor = now): Point[] {
  const out: Point[] = [];
  const from = now - DAYS * DAY;
  COWS.forEach((c, idx) => {
    const until = now - (LATE[c.id] ?? 0);
    const k0 = Math.ceil((from / 1000 - c.offset) / T), k1 = Math.floor((until / 1000 - c.offset) / T);
    let pendingCut: Point[] = [];
    for (let k = k0; k <= k1; k++) {
      const t = (k * T + c.offset) * 1000;
      if (c.missed && t >= anchor - c.missed[0] * HOUR && t <= anchor - c.missed[1] * HOUR) continue;
      const pos = positionOf(c, idx, t, anchor);
      const ll = toLL(pos.x, pos.y);
      const rssi = rssiAt(pos.x, pos.y) + (hash(k, idx) - 0.5) * 6;
      const daylight = Math.max(0, Math.sin((Math.PI * (new Date(t).getHours() - 6)) / 13));
      const mv = Math.round(c.base + 110 * daylight - 25);
      const p: Point = {
        tracker: c.id, seq: k, time: iso(t), lat: ll.lat, lon: ll.lon,
        alt_m: Math.round(terrain(pos.x, pos.y) * 10) / 10,
        sats: 8 + Math.floor(hash(k, idx + 3) * 6), hdop: Math.round((0.7 + hash(idx + 5, k) * 0.9) * 100) / 100,
        fix_age_s: Math.floor(hash(k + 1, idx) * 3), battery_mv: mv,
        battery: mv < 3540 ? "CRITICAL" : mv < 3650 ? "LOW" : "NORMAL",
        active_s: Math.round(pos.act * T), covered_s: T, source: "GNSS", period_s: T,
        rssi: null, snr: null, received: null, delay_s: null, link: null,
      };
      if (rssi < HEARD) { pendingCut.push(p); continue; }   // kept on the collar until heard again
      const arrive = t + (2 + Math.floor(hash(idx, k + 9) * 4)) * 1000;
      for (const q of pendingCut) {
        q.rssi = Math.round(rssi); q.snr = Math.round(snrOf(rssi) * 10) / 10;
        q.received = iso(arrive + 1000); q.delay_s = Math.round((arrive + 1000 - Date.parse(q.time!)) / 1000); q.link = "cut";
        out.push(q);
      }
      pendingCut = [];
      p.rssi = Math.round(rssi); p.snr = Math.round(snrOf(rssi) * 10) / 10;
      p.received = iso(arrive); p.delay_s = Math.round((arrive - t) / 1000); p.link = "live";
      out.push(p);
    }
    // Records still on a collar that nobody hears have not reached the phone.
  });
  return out;
}

export function demoBook(now: number): AssetBook {
  const book: AssetBook = { assets: {}, assignments: [] };
  for (const c of COWS) {
    book.assets[c.id] = {
      ...blankProfile(c.id), name: c.name, tag: c.tag, breed: c.breed, sex: c.sex, birth: c.birth, color: c.color,
      notes: (c.notes ?? []).map(([h, text]) => ({ at: now - h * HOUR, text })),
    };
    book.assignments.push({ device: c.id, asset: c.id, from: 0 });
  }
  return book;
}

function demoJournal(now: number, points: Point[]): JournalEntry[] {
  const j: JournalEntry[] = [];
  for (let d = 2; d >= 0; d--) {
    const day = new Date(now - d * DAY); day.setHours(7, 4, 0, 0);
    const at = day.getTime();
    if (at > now) continue;
    const n = points.filter((p) => { const t = Date.parse(p.time!); return t > at - DAY && t <= at; }).length;
    j.push({ at, kind: "link", text: "İstasyona bağlanıldı (Bluetooth)" });
    j.push({ at: at + 40_000, kind: "records", text: `${n} yeni kayıt istasyondan alındı`, n });
    j.push({ at: at + 55_000, kind: "handoff", text: `${n} kayıt telefona kalıcı yazıldı · istasyonda yer açıldı`, n });
    j.push({ at: at + 90_000, kind: "upload", text: `${n} kayıt sunucuya gönderildi`, n });
  }
  return j;
}

export class DemoSource implements EdgeSource {
  readonly kind = "demo" as const;
  private listeners = new Set<() => void>();
  private timer: ReturnType<typeof setInterval> | null = null;
  private anchor = Date.now();
  snap: Snapshot;

  constructor() {
    this.snap = this.build(Date.now(), 1);
  }

  private build(now: number, version: number): Snapshot {
    const pts = demoPoints(now, this.anchor);
    return {
      ready: true, error: null, link: "ble", state: "connected", detail: null,
      counts: { STORED: 38, DUPLICATE: 2, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 },
      records: pts.length, handedOff: 38, rejected: 0, pending: 0,
      gateway: DEMO_STATION.id, gatewayHeld: "2/1056", storage: "örnek veri (bellekte)",
      log: ["örnek sürü: kayıtlar telefonda üretildi, cihaz yok"],
      server: {
        url: "https://sunucu.orun.example", token: "örnek", auto: true, busy: false,
        counts: { uploaded: pts.length - 3, pending: 3, conflict: 0, rejected: 0 },
        lastOk: new Date(now - 4 * 60_000).toLocaleTimeString("tr-TR", { hour12: false }), error: null,
      },
      pointsVersion: version, journal: demoJournal(now, pts),
    };
  }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    if (!this.timer) this.timer = setInterval(() => {
      this.snap = this.build(Date.now(), this.snap.pointsVersion + 1);
      for (const l of this.listeners) l();
    }, 60_000);
    return () => {
      this.listeners.delete(fn);
      if (!this.listeners.size && this.timer) { clearInterval(this.timer); this.timer = null; }
    };
  }

  async points(): Promise<PointsResult> {
    const now = Date.now();
    const points = demoPoints(now, this.anchor);
    return { count: points.length, skipped: 0, now: iso(now), points };
  }

  async connectBle(): Promise<void> { /* the sample station is always connected */ }
  async connectUsb(): Promise<void> { /* sample */ }
  async disconnect(): Promise<void> { /* sample */ }
  saveServer(): void { /* sample */ }
  async sync(): Promise<void> { /* sample */ }
}

// Sample pastures: "Ev merası" holds the barn, the slope, the water and the
// shade; the stony ground the herd visits in the afternoon is outside it, so
// the sample shows an exit and a return. "Taşlık" is drawn but switched off.
export function demoZones(): Zone[] {
  const ring = (pts: [number, number][]) => pts.map(([x, y]) => { const p = toLL(x, y); return [p.lon, p.lat] as [number, number]; });
  return [
    { id: "demo-ev", name: "Ev merası", active: true, updated: 0,
      ring: ring([[-420, 80], [-200, -140], [180, -160], [560, 180], [560, 620], [100, 620], [-420, 420]]) },
    { id: "demo-taslik", name: "Taşlık", active: false, updated: 0,
      ring: ring([[-640, -480], [-220, -480], [-220, -80], [-640, -80]]) },
  ];
}
