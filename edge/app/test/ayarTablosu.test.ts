// Tek ayar tablosu: uygulamanın SETTINGS'i firmware'in settings_table.cpp
// satırlarıyla aynı (numara, ad); Bluetooth işlem numaraları oradan gelir;
// "APP SETTINGS?" satırları okunur.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { OP } from "../src/app/bleWrite.ts";
import { parseSettingLine, SETTINGS } from "../src/app/trackerSettings.ts";

const root = new URL("../../../", import.meta.url);
const read = (p: string) => readFileSync(new URL(p, root), "utf8");

test("uygulamanın ayar tablosu firmware'inkiyle aynı", () => {
  const kinds = new Map<string, number>();
  for (const m of read("firmware/include/config_mutation.h").matchAll(/(kSet\w+) = (\d+)/g))
    kinds.set(m[1], Number(m[2]));
  const rows = [...read("firmware/src/settings_table.cpp").matchAll(/\{ConfigMutationKind::(kSet\w+), "([A-Z_]+)",\s*([^}]*)\}/g)]
    .map((m) => ({ id: kinds.get(m[1]), name: m[2], rest: m[3] }));
  assert.equal(rows.length, Object.keys(SETTINGS).length);
  for (const s of Object.values(SETTINGS)) {
    const row = rows.find((r) => r.name === s.name);
    assert.ok(row, s.name);
    assert.equal(row.id, s.id, s.name);
    assert.equal(row.rest.includes("kSettingForGateway"), (s.roles as readonly string[]).includes("gateway"), s.name);
    assert.equal(row.rest.includes("kSettingForTracker"), (s.roles as readonly string[]).includes("tasma"), s.name);
  }
  assert.equal(OP.interval, SETTINGS.interval.id);
  assert.equal(OP.services, SETTINGS.services.id);
  assert.equal(OP.bleWindow, SETTINGS.bleWindow.id);
  // Area list ops are not settings rows.
  const ids: number[] = Object.values(SETTINGS).map((s) => s.id);
  assert.ok(!ids.includes(OP.fence) && !ids.includes(OP.fenceClear));
});

test("APP SETTINGS? satırları okunur", () => {
  assert.deepEqual(parseSettingLine("APP SETTING id=1 name=INTERVAL value=900 usb=1 ble=1 lora=0 radio=0"), {
    id: 1, name: "INTERVAL", value: 900, usb: true, ble: true, lora: false, radio: false,
  });
  assert.equal(parseSettingLine("APP SETTINGS END count=3 role=TRACKER"), null);
  assert.equal(parseSettingLine("APP SET id=1 code=APPLIED"), null);
});
