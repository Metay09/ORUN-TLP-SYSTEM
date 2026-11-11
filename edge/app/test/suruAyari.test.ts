// Ayarlar → Tasma ayarları: bir ayarın bütün sürüdeki özeti.
import assert from "node:assert/strict";
import test from "node:test";
import { fleetCommon, fleetLine, fleetRows, type DeviceSettingsEntry } from "../src/app/deviceSettings.ts";

const A = "0E8ADE7E71531AA3", B = "52E16773141A8887", C = "09A462BD4B275BA5";
const entry = (wanted: object, reported: object | null): DeviceSettingsEntry =>
  ({ wanted, wantedAt: 1, reported, reportedAt: reported ? 2 : null, reportedVia: reported ? "usb" : null } as DeviceSettingsEntry);
const interval = (rows: ReturnType<typeof fleetRows>) => rows.find((r) => r.def.key === "interval_s")!;

test("hepsi aynı ve cihazda: tek değer, hepsinde", () => {
  const r = interval(fleetRows([{ id: A }, { id: B }], {
    [A]: entry({}, { interval_s: 3600 }), [B]: entry({ interval_s: 3600 }, { interval_s: 3600 }),
  }));
  assert.equal(fleetCommon(r), 3600);
  assert.deepEqual(fleetLine(r), { value: "60 dk", sub: "2 tasmanın hepsinde", waiting: false });
});

test("istenen cihazdakinden farklı: bekliyor sayılır, değer istenen", () => {
  const r = interval(fleetRows([{ id: A }, { id: B }], {
    [A]: entry({ interval_s: 1800 }, { interval_s: 3600 }), [B]: entry({ interval_s: 1800 }, { interval_s: 1800 }),
  }));
  assert.equal(r.waiting, 1);
  const line = fleetLine(r);
  assert.equal(line.value, "30 dk");
  assert.equal(line.sub, "1 tasma bekliyor (LoRa'dan bir sonraki raporunda ya da bağlanınca)");
  assert.equal(line.waiting, true);
});

test("farklı değerler: karışık, dağılım çoktan aza; okunmayan ayrıca yazılır", () => {
  const r = interval(fleetRows([{ id: A }, { id: B }, { id: C, observed: { interval_s: 3600 } }, { id: "FFFFFFFFFFFFFFFF" }], {
    [A]: entry({}, { interval_s: 3600 }), [B]: entry({}, { interval_s: 900 }),
  }));
  assert.equal(fleetCommon(r), null);
  const line = fleetLine(r);
  assert.equal(line.value, "Karışık");
  assert.equal(line.sub, "2 tasma 60 dk · 1 tasma 15 dk · 1 tasmadan henüz okunmadı");
});

test("hiç bilgi yok: çizgi, ortak değer yok", () => {
  const r = interval(fleetRows([{ id: A }], {}));
  assert.equal(fleetCommon(r), null);
  assert.deepEqual(fleetLine(r), { value: "—", sub: "1 tasmadan henüz okunmadı", waiting: false });
});
