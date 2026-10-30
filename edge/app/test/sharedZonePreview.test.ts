// ORUN S6F - read-only projection tests. No APK, live SQLite or network.
import assert from "node:assert/strict";
import { test } from "node:test";
import { sharedZonePreviews } from "../src/app/sharedZonePreview.ts";
import type { SyncResource } from "../src/app/offlineSync.ts";

const square = [
  [376100000, 280500000],
  [376100000, 280510000],
  [376110000, 280510000],
  [376110000, 280500000],
];

function zone(name: string, ring_e7: unknown = square,
              active = true): SyncResource {
  return {
    revision: 3, deleted: false,
    body: { name, active, ring_e7 },
  };
}
function project(resources: Record<string, SyncResource>) {
  return sharedZonePreviews({ resources });
}

test("canonical E7 [lat,lon] gives read-only hectare preview without side effects", () => {
  const resources = {
    "zone:z1": zone("Kuzey Mera"),
    "group:g1": { revision: 1, deleted: false,
      body: { name: "Birinci", device_ids: [] } },
    "zone_target:z1": { revision: 1, deleted: false,
      body: { zone_id: "z1", scope: "all", device_ids: [], group_ids: [] } },
  } satisfies Record<string, SyncResource>;
  const before = JSON.stringify(resources);
  const result = project(resources);
  assert.equal(result.total, 1);
  assert.equal(result.invalid, 0);
  assert.deepEqual(result.entries.map((item) => [item.id, item.name, item.revision]),
    [["z1", "Kuzey Mera", 3]]);
  assert.equal(result.entries[0].vertices, 4);
  assert.ok(result.entries[0].hectares > 0.8 &&
            result.entries[0].hectares < 1.2);
  assert.equal("applied" in result.entries[0], false);
  assert.equal(JSON.stringify(resources), before,
    "projection must never edit local or canonical records");
});

test("malformed names, ranges, geometry and unknown zone keys are never displayed", () => {
  const resources: Record<string, SyncResource> = {
    "zone:valid": zone("Saglam"),
    "zone:bad-lat": zone("Outside", [[900000001,280500000], ...square.slice(1)]),
    "zone:crossing": zone("Cross", [square[0],square[2],square[1],square[3]]),
    "zone:bad-name": zone(""),
    "zone:short": zone("Short", square.slice(0, 2)),
    "zone:bad/key": zone("Wrong key"),
    "zone:removed": { revision: 4, deleted: true, body: null },
    "group:g1": zone("Not a zone"),
  };
  const result = project(resources);
  assert.equal(result.total, 1);
  assert.deepEqual(result.entries.map((item) => item.id), ["valid"]);
  assert.equal(result.invalid, 5);
});

test("missing, malformed or noninteger E7 vertices fail closed", () => {
  const resources: Record<string, SyncResource> = {
    "zone:null": { revision:1, deleted:false, body:null },
    "zone:float": zone("Fraction", [[376100000.5,280500000], ...square.slice(1)]),
    "zone:broken": zone("Broken", [["x",280500000], ...square.slice(1)]),
    "zone:duplicate": zone("Dup", [square[0], square[0], square[2], square[3]]),
    "zone:no-active": { revision: 1, deleted: false, body: {
      name: "No active", ring_e7: square,
    } },
  };
  const result = project(resources);
  assert.equal(result.total, 0);
  assert.equal(result.invalid, 5);
});

test("listing is deterministic and bounded without deleting hidden entries", () => {
  const resources: Record<string, SyncResource> = {};
  for (let i = 59; i >= 0; i--) {
    const id = String(i).padStart(2, "0");
    resources["zone:z" + id] = zone("Mera " + id);
  }
  const result = project(resources);
  assert.equal(result.total, 60);
  assert.equal(result.entries.length, 50);
  assert.equal(result.entries[0].id, "z00");
  assert.equal(result.entries.at(-1)?.id, "z49");
  assert.equal(Object.keys(resources).length, 60);
});

test("offline empty canonical mirror is empty, not evidence of uploaded or applied zones", () => {
  assert.deepEqual(project({}), { entries: [], total: 0, invalid: 0 });
});
