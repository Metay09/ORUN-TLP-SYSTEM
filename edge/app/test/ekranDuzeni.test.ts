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
