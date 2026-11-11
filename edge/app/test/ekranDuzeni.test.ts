// Ekran düzeni 2026-11-11: tasmaya açık kalan Bluetooth bağlantısı, hayvan
// kartının hareket karşılaştırması ve pil gidişatı, Bağlantı'daki tek cümlelik
// aktarım ve bekleyen işlemin sözcükleri, Olaylar'ın zaman çizelgesi satırı.
import assert from "node:assert/strict";
import test from "node:test";
import { activityBaseline, batteryOutlook, busiestHour } from "../src/app/activity.ts";
import { digest } from "../src/app/digest.ts";
import { opWhat } from "../src/app/deviceOps.ts";
import type { Animal, AppEvent, Observation } from "../src/app/model.ts";
import { trackerSession } from "../src/app/trackerSession.ts";
import { transferHeadline } from "../src/features/network/words.ts";
import type { TrackerBle } from "../src/platform/trackerBle.ts";

const HOUR = 3600_000, DAY = 24 * HOUR;

function fakeLink() {
  let gone: (() => void) | null = null, closed = 0;
  const link = {
    alive: true,
    async close() { link.alive = false; closed++; },
    exchange: async () => { throw new Error("unused"); },
    readRaw: async () => ({}),
  };
  const connect = async (_id: string, onGone: () => void) => { gone = onGone; return link as unknown as TrackerBle; };
  return { link, connect, drop: () => { link.alive = false; gone?.(); }, closed: () => closed };
}

test("tasma bağlantısı: pencere kapanınca kopmaz, aynı tasmaya yeniden açılmaz, kes deyince kapanır", async () => {
  const a = fakeLink();
  const l1 = await trackerSession.open("ble-1", "0e8ade7e71531aa3", a.connect);
  assert.equal(trackerSession.snapshot()?.device, "0E8ADE7E71531AA3");
  // Same tracker again (by ID or by Bluetooth device): the open link is reused.
  const b = fakeLink();
  assert.equal(await trackerSession.open("ble-1", null, b.connect), l1);
  assert.equal(trackerSession.find("0E8ADE7E71531AA3")?.bleId, "ble-1");
  assert.equal(b.closed(), 0);
  // Another tracker: the first one closes, only one stays open.
  const c = fakeLink();
  await trackerSession.open("ble-2", "52E16773141A8887", c.connect);
  assert.equal(a.closed(), 1);
  assert.equal(trackerSession.find("0E8ADE7E71531AA3"), null);
  // The tracker walks away: the session is gone without anyone closing it.
  c.drop();
  assert.equal(trackerSession.snapshot(), null);
  const d = fakeLink();
  await trackerSession.open("ble-3", null, d.connect);
  await trackerSession.close();
  assert.equal(d.closed(), 1);
  assert.equal(trackerSession.snapshot(), null);
});

const obs = (t: number, active: number, period = 900): Observation =>
  ({ tracker: "A", seq: Math.round(t / 1000), time: new Date(t).toISOString(), active_s: active, covered_s: period, period_s: period } as unknown as Observation);
function days(end: number, n: number, share: (day: number) => number): Observation[] {
  const out: Observation[] = [];
  for (let t = end - n * DAY + 900_000; t <= end; t += 900_000) out.push(obs(t, Math.round(900 * share(Math.floor((end - t) / DAY)))));
  return out;
}

test("hareket karşılaştırması: hayvanın kendi günlerine göre, az veriyle yok", () => {
  const end = Date.UTC(2026, 10, 11, 12);
  const same = activityBaseline(days(end, 8, () => 0.3), end)!;
  assert.ok(Math.abs(same.change) < 0.01);
  assert.equal(same.days, 7);
  const less = activityBaseline(days(end, 8, (d) => (d === 0 ? 0.15 : 0.3)), end)!;
  assert.ok(Math.abs(less.change + 0.5) < 0.02, "bugün yarı yarıya az");
  assert.equal(activityBaseline(days(end, 2, () => 0.3), end), null, "3 tam gün yoksa karşılaştırma yok");
  assert.equal(busiestHour([null, 0.2, 0.9, 0.01]), 2);
  assert.equal(busiestHour([null, 0.01, null]), null);
});

test("pil gidişatı: düşüşte kalan gün, dengede gün yok, gündüz şarjı", () => {
  const now = Date.UTC(2026, 10, 11, 12);
  // 40 % three days ago, -2 % per day.
  const fixed = Array.from({ length: 73 }, (_, i) => ({ t: now - (72 - i) * HOUR, p: 40 - (2 * i) / 24 }));
  const o = batteryOutlook(fixed, now);
  assert.ok(Math.abs(o.perDay! + 2) < 0.01);
  assert.ok(Math.abs(o.daysLeft! - 17) < 0.1);
  const solar = Array.from({ length: 73 }, (_, i) => ({ t: now - (72 - i) * HOUR, p: 60 + 8 * Math.sin((2 * Math.PI * i) / 24) }));
  const s = batteryOutlook(solar, now);
  assert.equal(s.daysLeft, null, "güneş harcananı karşılıyorsa kalan gün yok");
  assert.ok(s.charged24h! >= 7, "dipten tepeye yükseliş");
  assert.deepEqual(batteryOutlook(solar.slice(-5), now).perDay, null, "az veriyle gidişat yok");
});

test("aktarım tek cümle: giriş, hata, bekleyen, son kayıt", () => {
  const sync = { running: false, last: null, error: null };
  const now = 2_000_000_000_000;
  assert.match(transferHeadline({ signedIn: false, sync, waiting: 5, lastUpload: null, now }).text, /giriş/);
  assert.equal(transferHeadline({ signedIn: true, sync: { ...sync, error: "x" }, waiting: 0, lastUpload: null, now }).tone, "alarm");
  assert.equal(transferHeadline({ signedIn: true, sync, waiting: 37, lastUpload: now, now }).text, "37 kayıt sunucuya gitmeyi bekliyor.");
  const ok = transferHeadline({ signedIn: true, sync, waiting: 0, lastUpload: now - 4 * 60_000, now });
  assert.equal(ok.tone, "fresh");
  assert.match(ok.text, /4 dk önce/);
});

test("bekleyen işlem sözcükleri", () => {
  assert.equal(opWhat({ kind: "fence", payload: { digest: "", areas: 1, vertices: 4, zones: [] } }), "Alanlar");
  assert.equal(opWhat({ kind: "settings", payload: { set: { interval_s: 1800, tracking: true } } }), "Rapor aralığı: 30 dk, Konum takibi: Açık");
});

test("zaman çizelgesi satırı türünü taşır (simge için)", () => {
  const NOW = new Date(2026, 10, 10, 18).getTime();
  const herd = [{ id: "A", name: "Duman", batteryPercent: 80, last: null } as unknown as Animal];
  const e = (kind: AppEvent["kind"], at: number, end: number): AppEvent =>
    ({ id: kind + at, kind, severity: "warn", asset: "A", device: null, title: "", detail: "", at, end, arrivedAt: null, alert: true });
  const d = digest([e("silent", NOW - 5 * HOUR, NOW - 3 * HOUR), e("still", NOW - 9 * HOUR, NOW - 6 * HOUR)], herd, NOW);
  assert.deepEqual(d.days[0].lines.map((l) => l.family).sort(), ["gap", "still"]);
});

import { transferHealth, EDGE_ALIVE_MS } from "../src/features/network/transferFlow.ts";

test("aktarım sağlığı: tasma/gateway bekleyenleri ölçüm zamanıyla, Edge ve sunucu çalışıyor mu", () => {
  const now = 2_000_000_000_000, MIN = 60_000;
  const h = transferHealth({
    now,
    trackers: [{ id: "0E8ADE7E71531AA3", lastTime: now - 5 * MIN, periodS: 900 }, { id: "52E16773141A8887", lastTime: now - 5 * HOUR, periodS: 900 }],
    collars: [{ device: "0E8ADE7E71531AA3", count: 120, capacity: 2600, at: now - 2 * HOUR, via: "ble" }],
    gateways: [{ gateway: "09A462BD4B275BA5", via: "pc-usb", held: 3, capacity: 1056, lastSeen: now - MIN, statusAt: now - 2 * MIN, lastUpload: now - MIN, trackers24h: 2 }],
    localGateway: { id: null, link: "none", connected: false, held: null, capacity: null, readAt: null },
    edges: [{ edge: "pc-1", pending: 4, delivered: 900, quarantined: 0, lastSeen: now - 20_000 },
            { edge: "pc-eski", pending: 7, delivered: 10, quarantined: 1, lastSeen: now - EDGE_ALIVE_MS - 1 }],
    phone: { pendingUpload: null },
    server: { signedIn: true, running: false, lastSync: now - MIN, error: null },
    label: (id) => (id === "0E8ADE7E71531AA3" ? "Sarıkız" : id),
  });
  const [tr_, gw, edge, server] = h;
  assert.equal(tr_.line, "1/2 tasmadan kayıt geliyor");
  assert.equal(tr_.tone, "aging");
  assert.match(tr_.items.find((x) => x.title === "Sarıkız")!.line, /bellekte 120 kayıt \(Bluetooth ile 2 sa önce okundu\)/);
  assert.equal(gw.line, "1/1 bağlı · 3 kayıt bekliyor");
  assert.match(gw.items[0].line, /PC Edge'e USB ile bağlı · 3 kayıt bekliyor \(2 dk önce ölçüldü\) · son gönderim 1 dk önce/);
  assert.equal(edge.line, "1/2 çalışıyor");
  assert.equal(edge.tone, "alarm");
  assert.match(edge.items.find((x) => x.id === "pc-eski")!.line, /^çalışmıyor/);
  assert.match(server.line, /^Çalışıyor · son eşitleme/);
  assert.equal(server.items.length, 0, "sunucunun toplam sayısı yok");
});

test("aktarım sağlığı: telefon köprü olunca gateway ve köprü satırı telefondan", () => {
  const now = 2_000_000_000_000;
  const [, gw, edge] = transferHealth({
    now, trackers: [], collars: [], gateways: null,
    localGateway: { id: "09a462bd4b275ba5", link: "ble", connected: true, held: 37, capacity: 2600, readAt: now },
    edges: null, phone: { pendingUpload: 0 },
    server: { signedIn: false, running: false, lastSync: null, error: null }, label: (id) => id,
  });
  assert.match(gw.items[0].line, /^bu telefona Bluetooth ile bağlı · 37 kayıt bekliyor/);
  assert.equal(edge.items[0].title, "Bu telefon");
});

import { notArrived } from "../src/features/network/transferFlow.ts";

test("henüz ulaşmayan kayıt: sıra numarasındaki boşluklar, en yeni dönem, konumsuz kayıt boşluk değil", () => {
  const now = Date.UTC(2026, 10, 11, 12);
  const rec = (seq: number, minAgo: number, incarnation = "B") => ({ seq, incarnation, time: new Date(now - minAgo * 60_000).toISOString() });
  // 100..110 made; 103..106 still on the collar; 108 had no fix but arrived (unlocated list).
  const recs = [100, 101, 102, 107, 108, 109, 110].map((s) => rec(s, (110 - s) * 15));
  const r = notArrived(recs, now);
  assert.equal(r.missing, 4);
  assert.equal(r.since, now - 8 * 15 * 60_000, "boşluk 102'den sonra başlıyor");
  assert.equal(notArrived(recs.filter((x) => x.seq !== 108), now).missing, 5, "konumsuz kayıt gelmezse o da eksik sayılır");
  // An older incarnation (re-enrolled collar) is not mixed in.
  assert.equal(notArrived([...recs, rec(5, 3000, "A"), rec(9, 2990, "A")], now).missing, 4);
  assert.deepEqual(notArrived([rec(1, 5)], now), { missing: 0, since: null });
});

import { buildHerd } from "../src/app/herd.ts";
import { deriveEvents } from "../src/app/events.ts";

test("konumsuz rapor: tasma 'haber yok' değil 'konum bulamıyor'; aradaki boşluk sessizlik sayılmaz", () => {
  const now = Date.UTC(2026, 10, 11, 2, 20);
  const MIN = 60_000;
  const pt = (minAgo: number, seq: number) => ({
    tracker: "0E8ADE7E71531AA3", incarnation: "I", seq, time: new Date(now - minAgo * MIN).toISOString(),
    lat: 37.6, lon: 28.05, alt_m: null, sats: 7, hdop: 1, fix_age_s: 0, battery_mv: 4000, battery: "NORMAL",
    active_s: null, covered_s: null, source: "GNSS", period_s: 900, rssi: -70, snr: 5, received: null, delay_s: null, link: null,
  }) as unknown as Observation;
  const points = [pt(15 * 60, 1), pt(13 * 60, 2)];          // last fix 13 h ago
  const unlocated = Array.from({ length: 52 }, (_, k) => ({ tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: 3 + k,
    time: new Date(now - (13 * 60 - 15 * (k + 1)) * MIN).toISOString() }));
  const [a] = buildHerd({ points, profiles: {}, assetOf: (d) => d, now, unlocated });
  assert.equal(a.noFix, true);
  assert.equal(a.status, "aging");
  assert.match(a.seen, /^Konum bulamıyor · son rapor/);
  assert.match(a.seen, /· son konum /);
  const ev = deriveEvents({ herd: [a], station: { id: null, link: "none", phase: "idle", detail: null, held: null, capacity: null, position: null },
    server: { configured: false, url: "", token: "", auto: true, busy: false, uploaded: null, pending: null, conflict: 0, rejected: 0, lastOk: null, error: null },
    journal: [], coverage: null, now });
  const open = ev.filter((e) => e.alert && e.end === null);
  assert.equal(open.length, 1);
  assert.ok(open[0].id.startsWith("nofix:"));
  assert.ok(!ev.some((e) => e.kind === "lost"), "rapor geldiği için kayıp değil");
  // Without the reports it is silent / lost, as before.
  const [b] = buildHerd({ points, profiles: {}, assetOf: (d) => d, now });
  assert.equal(b.status, "lost");
});

test("GPS hiç çalışmayan tasma (kayıt saati yok, GPS hatası): 'GPS çalışmıyor', gateway'e ulaştığı saatle", () => {
  const now = Date.UTC(2026, 10, 11, 3, 10);
  const MIN = 60_000;
  const points = [{
    tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: 1, time: new Date(now - 13 * 60 * MIN).toISOString(),
    lat: 37.6, lon: 28.05, alt_m: null, sats: 7, hdop: 1, fix_age_s: 0, battery_mv: 4000, battery: "NORMAL",
    active_s: null, covered_s: null, source: "GNSS", period_s: 120, rssi: -70, snr: 5, received: null, delay_s: null, link: null,
  } as unknown as Observation];
  // Records as the server holds them on 2026-11-11: period_end 0 (no GNSS time), gps failures 1, every ~32 min.
  const unlocated = [2, 3, 4].map((k) => ({ tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: k, time: null,
    received: new Date(now - (4 - k) * 32 * MIN).toISOString(), gnssFailures: 1 }));
  const [a] = buildHerd({ points, profiles: {}, assetOf: (d) => d, now, unlocated });
  assert.equal(a.gpsFault, true);
  assert.equal(a.lastReport, now);
  assert.match(a.seen, /^GPS çalışmıyor · son rapor az önce · son konum/);
});

import { walkedM, smoothBattery } from "../src/app/activity.ts";

test("yürünen yol: masada duran tasmanın GPS oynaması yol sayılmaz, gerçek yürüyüş sayılır", () => {
  const t0 = Date.UTC(2026, 10, 11, 0, 0);
  const at = (k: number, lat: number, lon: number, hdop = 2, active = 30) => ({
    t: t0 + k * 120_000,
    p: { lat, lon, hdop, sats: 6, active_s: active, covered_s: 120 } as unknown as Observation,
  });
  // 24 h, every 2 min, jitter up to ~20 m around one spot (what the desk collar did).
  let seed = 7; const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647 - 0.5);
  const still = Array.from({ length: 720 }, (_, k) => at(k, 37.61007 + rnd() * 0.00035, 28.05399 + rnd() * 0.00045));
  const naive = still.slice(1).reduce((s, x, i) => s + Math.hypot((x.p.lat - still[i].p.lat) * 111_320, (x.p.lon - still[i].p.lon) * 88_200), 0);
  assert.ok(naive > 8000, "eski hesap kilometreler toplardı");
  assert.ok(walkedM(still) < 300, `durana yol yazılmaz (${Math.round(walkedM(still))} m)`);
  // A real walk: 1 km north in 20 min (10 fixes), then still.
  const walk = Array.from({ length: 11 }, (_, k) => at(k, 37.61 + (k * 100) / 111_320, 28.05));
  assert.ok(Math.abs(walkedM(walk) - 1000) < 30);
  // Felt no movement at all (active_s = 0): a jumped fix is not a walk.
  assert.equal(walkedM([at(0, 37.61, 28.05, 2, 0), at(1, 37.6106, 28.05, 2, 0)]), 0);
});

test("pil: tek düşük okuma şarj ya da düşüş sayılmaz", () => {
  const s = Array.from({ length: 20 }, (_, i) => ({ t: i, p: i === 10 ? 51 : 100 }));
  assert.ok(smoothBattery(s).every((x) => x.p === 100));
});

import { BatteryCapacityBook, expectedLife, lifeWord, measuredLife, timeOnAirS } from "../src/app/batteryLife.ts";

test("pil ömrü: beklenen (üretici değerleri), ölçülen (günlük düşüş), mAh tasma başına", () => {
  // 100-byte ORUN frame at SF11/125 kHz/4:5: ~2.2 s on air.
  assert.ok(Math.abs(timeOnAirS(100) - 2.22) < 0.05, String(timeOnAirS(100)));
  // 1000 mAh, one report an hour: today's firmware (no sleep) ~2 weeks, with real sleep ~4 months.
  const awake = expectedLife(1000, 3600, false), sleep = expectedLife(1000, 3600, true);
  assert.ok(awake.avgMa > 2.3 && awake.avgMa < 2.6, String(awake.avgMa));
  assert.ok(awake.days > 12 && awake.days < 15, String(awake.days));
  assert.ok(sleep.days > 100 && sleep.days < 140, String(sleep.days));
  // Under 60 s the GPS stays on: about 25 mA more.
  assert.ok(expectedLife(1000, 40, false).avgMa > 27);
  // Measured: 1000 mAh, -5 %/day → 50 mAh/day, full battery (80 %) 16 days; 50 % left → 6 days.
  const m = measuredLife(1000, -5, 50)!;
  assert.deepEqual([Math.round(m.mAhPerDay), Math.round(m.daysFull), Math.round(m.daysLeft!)], [50, 16, 6]);
  assert.equal(measuredLife(1000, 0.2, 100), null, "düşmeyen pilde ölçülen yok");
  assert.deepEqual([lifeWord(1.5), lifeWord(13.6), lifeWord(119), lifeWord(900)], ["~36 saat", "~14 gün", "~4 ay", "~2,5 yıl"]);
  const store = new Map<string, string>();
  const book = new BatteryCapacityBook({ getItem: (k) => store.get(k) ?? null, setItem: (k, v) => { store.set(k, v); } });
  book.set("0e8ade7e71531aa3", 3200);
  assert.equal(new BatteryCapacityBook({ getItem: (k) => store.get(k) ?? null, setItem: () => {} }).get("0E8ADE7E71531AA3"), 3200);
  assert.throws(() => book.set("0E8ADE7E71531AA3", 10));
});

test("konumsuz rapor da rapordur: pil ve hareket ondan da okunur; saatsiz kayıtta pil gateway'e ulaştığı saatle", () => {
  const now = Date.UTC(2026, 10, 11, 5, 16);
  const MIN = 60_000;
  const points = [{
    tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: 1, time: new Date(now - 13 * 60 * MIN).toISOString(),
    lat: 37.6, lon: 28.05, alt_m: null, sats: 4, hdop: 4, fix_age_s: 0, battery_mv: 4050, battery: "NORMAL",
    active_s: 10, covered_s: 120, source: "GNSS", period_s: 540, rssi: -70, snr: 5, received: null, delay_s: null, link: null,
  } as unknown as Observation];
  const unlocated = [
    { tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: 2, time: new Date(now - 60 * MIN).toISOString(),
      battery_mv: 4000, battery: "NORMAL", active_s: 30, covered_s: 120, period_s: 120 },
    { tracker: "0E8ADE7E71531AA3", incarnation: "I", seq: 3, time: null, received: new Date(now - 30 * MIN).toISOString(),
      gnssFailures: 1, battery_mv: 3990, battery: "NORMAL", active_s: 0, covered_s: 120, period_s: 1920 },
  ];
  const [a] = buildHerd({ points, profiles: {}, assetOf: (d) => d, now, unlocated });
  assert.equal(a.batteryAt, now - 30 * MIN, "pil en yeni rapordan");
  assert.equal(a.batteryMv, 3990);
  assert.equal(a.batteryTrail.length, 3);
  assert.deepEqual(a.reports.map((r) => r.seq), [1, 2], "hareket için saati olan bütün raporlar");
  assert.equal(a.points.length, 1, "haritada yalnız konumlu kayıt");
});
