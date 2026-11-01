import assert from "node:assert/strict";
import { test } from "node:test";
import type { Zone } from "../src/app/zones.ts";

const ring: [number,number][] = [
  [28.0540,37.6100], [28.0551,37.6100],
  [28.0551,37.6109], [28.0540,37.6109],
];
const legacy = [{ id: "old-mera", name: "Önceki mera", active: true, ring }];
const key = "orun.app.zones.v1";
const old = "orun.fences.v1";

function stubStorage(initial: Record<string,string>, writeFails = false) {
  const m = new Map(Object.entries(initial));
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true, value: {
      getItem: (k: string) => m.get(k) ?? null,
      setItem: (k: string, v: string) => { if (writeFails) throw new Error("Disk full"); m.set(k, v); },
      removeItem: (k: string) => { m.delete(k); },
    },
  });
  return m;
}

test("legacy ORUN-Saha and PR180 pasture drafts load unchanged and original is kept", async () => {
  const source = JSON.stringify(legacy);
  const data = stubStorage({ [old]: source });
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  assert.equal(z.migrationWarning, null);
  assert.equal(z.get().zones[0].name, "Önceki mera");
  assert.equal(data.get(old), source, "old record is never overwritten");
  assert.equal(data.get(key), undefined, "read-only migration before first save");
  z.save({ ...z.get().zones[0], name: "Yeni mera" });
  assert.equal(JSON.parse(data.get(key)!).zones[0].name, "Yeni mera");
  assert.equal(data.get(old), source, "old backup survives update");
});

test("corrupt legacy zones block writes and stay recoverable", async () => {
  const data = stubStorage({ [old]: "{broken" });
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  assert.ok(z.migrationWarning);
  assert.throws(() => z.save({ id: "new", name: "New", active: true, ring, updated: 0 }), /okunamadı/);
  assert.equal(data.get(old), "{broken");
  assert.equal(data.get(key), undefined);
});

test("existing new-format zones take precedence, even when legacy data also exists", async () => {
  const data = stubStorage({ [old]: "{broken",
    [key]: JSON.stringify({ zones: [{ ...legacy[0], name: "Yeni şema", updated: 10 }],
      graze: { until: null, forever: false } }),
  });
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  assert.equal(z.get().zones[0].name, "Yeni şema");
  assert.equal(z.migrationWarning, null);
  assert.ok(data.get(old));
});

test("invalid or over-capacity active zone sets never reach disk or memory", async () => {
  const data = stubStorage({});
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  const nine: Zone[] = Array.from({ length: 9 }, (_, i) =>
    ({ id: `zone-${i}`, name: `Mera ${i}`, active: true, ring, updated: 1 }));
  assert.throws(() => z.add(nine), /En fazla 8/);
  assert.equal(z.get().zones.length, 0);
  assert.equal(data.get(key), undefined);
});

test("disk writes must finish before reporting a saved zone", async () => {
  const data = stubStorage({}, true);
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  assert.throws(() => z.save({ id: "a", name: "A", active: true, ring, updated: 0 }), /Disk full/);
  assert.equal(z.get().zones.length, 0);
  assert.equal(data.get(key), undefined);
});

test("corrupt current schema never gets replaced silently by an empty list", async () => {
  const data = stubStorage({ [key]: "{broken" });
  const { ZoneStore } = await import("../src/app/stores.ts");
  const z = new ZoneStore(key);
  assert.match(z.migrationWarning!, /korunuyor/);
  assert.throws(() => z.save({ id: "a", name: "A", active: true, ring, updated: 0 }), /bozuk/);
  assert.equal(data.get(key), "{broken");
});
