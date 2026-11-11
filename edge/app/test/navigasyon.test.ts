// #262 regression: tab contract, Android back order and map -> area route.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { backFallback, type Tab } from "../src/app/nav.ts";

const read = (path: string) => readFileSync(new URL(path, import.meta.url), "utf8");

test("Android geri: üst ekran kapanır, sonra Harita, en son arka plan", () => {
  for (const tab of ["map", "herd", "events", "network", "settings"] as Tab[]) {
    assert.equal(backFallback(tab, 1), "pop");
    assert.equal(backFallback(tab, 2), "pop");
  }
  assert.equal(backFallback("settings", 0), "map");
  assert.equal(backFallback("network", 0), "map");
  assert.equal(backFallback("map", 0), "minimize");
});

test("alt çubuk: tam beş sekme ve doğru sırada", () => {
  const app = read("../src/App.tsx");
  const tabs = [...app.matchAll(/\{ tab: "([^"]+)", label: tr\.tabs\./g)].map((m) => m[1]);
  assert.deepEqual(tabs, ["map", "herd", "events", "network", "settings"]);
  assert.match(app, /if \(runBack\(\)\) return;/);
  assert.match(app, /backFallback\(tab, stack\.length\)/);
  assert.match(app, /if \(action === "pop"\) pop\(\)/);
  assert.match(app, /r\.to === "zones" && <ZonesScreen \/>/);
  assert.doesNotMatch(app, /tab === "zones"/);
});

test("Alanlar: haritadaki görünür düğmeden açılır, geri tuşuyla kapanır", () => {
  const map = read("../src/features/map/MapScreen.tsx");
  const zones = read("../src/features/zones/ZonesScreen.tsx");
  const nav = read("../src/app/nav.ts");
  assert.match(map, /onClick=\{\(\) => nav\.push\(\{ to: "zones" \}\)\}/);
  assert.match(map, /<span>\{tr\.zones\.title\}<\/span>/);
  assert.match(nav, /\| \{ to: "zones" \}/);
  assert.match(zones, /useBack\(\(\) => nav\.pop\(\), nav\.stack\[nav\.stack\.length - 1\]\?\.to === "zones"\)/);
  assert.match(zones, /onBack=\{\(\) => nav\.pop\(\)\}/);
});

test("kapalı harita panelleri üst ekranın Android geri tuşunu yutmaz", () => {
  const map = read("../src/features/map/MapScreen.tsx");
  assert.match(map, /visible && nav\.stack\.length === 0 && panel/);
  assert.match(map, /visible && nav\.stack\.length === 0 && !drawing/);
});

test("kullanıcı yönlendirmelerinde eski Cihazlar adı kalmaz", () => {
  for (const path of [
    "../src/features/zones/ZonesScreen.tsx",
    "../src/features/devices/DeviceScreen.tsx",
  ]) {
    assert.doesNotMatch(read(path), /Cihazlar\s*→/, path);
  }
  assert.match(read("../src/features/zones/ZonesScreen.tsx"), /Bağlantı → Bekleyen işlemler/);
  assert.match(read("../src/features/devices/DeviceScreen.tsx"), /Ayarlar → Tasma ayarları/);
});
