// Uygulama düzeni: Bağlantı sekmesinin rengi ve tek ad kuralı.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { connectionTone } from "../src/features/network/words.ts";

const base = { gatewayConnected: false, signedIn: true, syncError: false, lastSyncAt: null, now: 1_000_000_000 };

test("Bağlantı rengi: gateway bağlı ya da yeni eşitleme yeşil, hiç veri yolu yoksa kırmızı", () => {
  assert.equal(connectionTone({ ...base, gatewayConnected: true, signedIn: false }), "ok");
  assert.equal(connectionTone({ ...base, lastSyncAt: base.now - 60_000 }), "ok");
  assert.equal(connectionTone({ ...base, lastSyncAt: base.now - 10 * 60_000 }), "wait");
  assert.equal(connectionTone(base), "wait");
  assert.equal(connectionTone({ ...base, signedIn: false }), "bad");
  assert.equal(connectionTone({ ...base, syncError: true, lastSyncAt: base.now - 1000 }), "bad");
});

test("ekranlarda tek ad: İstasyon değil Gateway, Mera değil Alan", () => {
  const text = readFileSync(new URL("../src/app/i18n/tr.ts", import.meta.url), "utf8");
  assert.doesNotMatch(text, /İstasyon|istasyon/);
  assert.doesNotMatch(text, /"Mera|mera sınır/);
});

import { probeUsbDevice } from "../src/app/addTracker.ts";
import { parseFarmGateways } from "../src/app/ownerAccountClient.ts";
import { gatewayFacts, gatewayLine, serverLine } from "../src/features/network/words.ts";
import { classifyNearby, mergeNearby } from "../src/platform/bleScan.ts";

test("yakındaki cihazlar: ORUN-adının son 8 hanesi gateway'i ya da hayvanı bulur", () => {
  const gws = ["09A462BD4B275BA5"];
  const herd = [{ id: "0e8ade7e71531aa3", name: "Sarıkız" }];
  const g = classifyNearby({ deviceId: "a", name: "ORUN-4B275BA5", rssi: -60 }, gws, herd);
  assert.deepEqual([g.kind, g.label, g.id], ["gateway", "ORUN-4B275BA5", "09A462BD4B275BA5"]);
  assert.equal(classifyNearby({ deviceId: "a", name: "ORUN-4B275BA5", rssi: -60 }, gws, herd, () => "Tepe").label, "Tepe");
  const t = classifyNearby({ deviceId: "b", name: "ORUN-71531AA3", rssi: -70 }, gws, herd);
  assert.deepEqual([t.kind, t.label], ["tracker", "Sarıkız"]);
  const u = classifyNearby({ deviceId: "c", name: "ORUN-DEADBEEF", rssi: null }, gws, herd);
  assert.equal(u.kind, "unknown");
  let list = mergeNearby([], { deviceId: "a", name: "ORUN-1", rssi: -80 });
  list = mergeNearby(list, { deviceId: "b", name: "ORUN-2", rssi: -50 });
  list = mergeNearby(list, { deviceId: "a", name: "ORUN-1", rssi: -40 });
  assert.deepEqual(list.map((d) => [d.deviceId, d.rssi]), [["a", -40], ["b", -50]]);
});

test("diğer gateway'ler: sunucu listesi ve satırlar", () => {
  const now = 1_800_000_000_000;
  const [pc, old] = parseFarmGateways({ gateways: [
    { gateway: "09A462BD4B275BA5", via: "pc-usb", held: 2, capacity: 1056, last_seen_utc: now / 1000 - 120, trackers_24h: 3 },
    { gateway: "AABBCCDDEEFF0011", via: null, held: null, capacity: null, last_seen_utc: now / 1000 - 3 * 3600, trackers_24h: 0 },
  ] });
  assert.match(gatewayLine(pc, now), /^PC'ye USB ile bağlı · 2 dk önce/);
  assert.equal(gatewayFacts(pc), "Bellek 2 / 1.056 · 3 tasma (24 sa)");
  assert.match(gatewayLine(old, now), /^Bağlı değil · son 3 sa/);
  assert.equal(gatewayFacts(old), "0 tasma (24 sa)");
  assert.throws(() => parseFarmGateways({ gateways: [{ gateway: "../x" }] }));
});

test("sunucu satırı: giriş yok, eşitleniyor, son eşitleme, bekleyen kayıt", () => {
  const idle = { running: false, last: null, error: null };
  assert.match(serverLine(false, idle, null, null), /giriş/);
  assert.equal(serverLine(true, { ...idle, running: true }, null, null), "Eşitleniyor…");
  const last = { latest: 2, history: 0, more: false, at: Date.UTC(2026, 10, 1, 13, 20) };
  assert.match(serverLine(true, { ...idle, last }, 5, null), /Son eşitleme .* · 2 tasma · 5 kayıt gönderilecek/);
});

test("USB ile bağlı cihaz: gateway ya da tasma", async () => {
  const fake = (lines: string[]) => ({
    async send() {},
    async waitFor<T>(match: (l: string) => T | null): Promise<T | null> {
      for (const l of lines) { const r = match(l); if (r) return r; }
      return null;
    },
  });
  assert.deepEqual(await probeUsbDevice(fake(["BENCH GATEWAY id=09A462BD4B275BA5 custody_region=READY held=2/1056 role=BASE"])),
    { kind: "gateway", id: "09A462BD4B275BA5" });
  const t = await probeUsbDevice(fake([
    "BENCH CREDENTIAL credential=" + "A".repeat(32) + " epoch=00000001 grant_generation=00000002 custody_floor=00000000",
    "BENCH TRACKER id=0E8ADE7E71531AA3 role=TRACKER granted_gateway=09A462BD4B275BA5",
  ]));
  assert.equal(t.kind, "tracker");
  assert.equal(t.kind === "tracker" && t.report.device, "0E8ADE7E71531AA3");
  await assert.rejects(probeUsbDevice(fake([])), /yanıt vermedi/);
});

import { DeviceNameBook, bleName, cleanName } from "../src/app/deviceNames.ts";
import { buildHerd } from "../src/app/herd.ts";

test("cihaz adları: hesaptaki ad hayvanın adı, adsız cihaz Bluetooth adıyla görünür", () => {
  assert.equal(bleName("09a462bd4b275ba5"), "ORUN-4B275BA5");
  assert.equal(cleanName("  Tepe \u0007  gateway  "), "Tepe gateway");
  const p = { tracker: "0e8ade7e71531aa3", seq: 1, time: new Date(1_800_000_000_000).toISOString(), lat: 37.6, lon: 28.05,
              period_s: 900, battery_mv: 4000, active_s: null, covered_s: null } as never;
  const base = { points: [p], profiles: {}, assetOf: (d: string) => d, now: 1_800_000_060_000 };
  assert.equal(buildHerd(base)[0].name, "ORUN-71531AA3");
  const [a] = buildHerd({ ...base, names: { "0E8ADE7E71531AA3": "Sarıkız" } });
  assert.deepEqual([a.name, a.named], ["Sarıkız", true]);
});

test("ad defteri: hemen görünür, gönderilemeyen ad sonra gönderilir, hesap adları gelir", async () => {
  const mem = new Map<string, string>();
  const store = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => { mem.set(k, v); } };
  const sent: [string, string][] = [];
  let online = false;
  let remote: Record<string, string> = { "AABBCCDDEEFF0011": "Ev" };
  const server = {
    async deviceNames() { if (!online) throw new Error("offline"); return { ...remote }; },
    async setDeviceName(d: string, n: string) {
      if (!online) throw new Error("offline");
      sent.push([d, n]);
      if (n) remote[d] = n; else delete remote[d];
    },
  };
  const book = new DeviceNameBook(store);
  await book.set("09a462bd4b275ba5", "Tepe", server);            // internet yok
  assert.equal(book.label("09A462BD4B275BA5"), "Tepe");
  assert.equal(book.label("1122334455667788"), "ORUN-55667788");
  await assert.rejects(book.refresh(server));
  assert.equal(new DeviceNameBook(store).name("09A462BD4B275BA5"), "Tepe", "telefonda kaldı");
  online = true;
  await book.refresh(server);
  assert.deepEqual(sent, [["09A462BD4B275BA5", "Tepe"]]);
  assert.deepEqual(book.snapshot(), { "AABBCCDDEEFF0011": "Ev", "09A462BD4B275BA5": "Tepe" });
  await book.set("09A462BD4B275BA5", "", server);                 // ad silinir
  assert.equal(book.label("09A462BD4B275BA5"), "ORUN-4B275BA5");
  assert.deepEqual(remote, { "AABBCCDDEEFF0011": "Ev" });
});
