// Telefon USB/Bluetooth yazımından sonra tasmanın yeni sürümünü bildirir.
import assert from "node:assert/strict";
import { test } from "node:test";
import { bleRevision, onBleRevision, revisionFromUsbLine, tapUsbRevisions } from "../src/app/loraRevision.ts";

test("USB satırlarından sürüm: ayar 1, alan 2; token yoksa hiçbir şey", () => {
  assert.deepEqual(revisionFromUsbLine("APP SET id=7 code=APPLIED tracking_interval_seconds=300 token=VALID revision=12"),
    { resource: 1, revision: 12 });
  assert.deepEqual(revisionFromUsbLine("APP SET id=8 code=UNCHANGED services=T mode=EXPLICIT token=VALID revision=13"),
    { resource: 1, revision: 13 });
  assert.deepEqual(revisionFromUsbLine("APP FENCE id=9 code=APPLIED state=CLEAR areas=0 vertices=0 token=VALID revision=5"),
    { resource: 2, revision: 5 });
  assert.equal(revisionFromUsbLine("APP SET id=7 code=APPLIED tracking_interval_seconds=300 token=UNAVAILABLE"), null);
  assert.equal(revisionFromUsbLine("APP GEOFENCE id=1 resource=1 token=1 areas=0"), null);
});

test("USB konsolu dinlenir; sonuç satırı yine yazana gider", async () => {
  const lines = ["APP SET id=1 code=APPLIED tracking_interval_seconds=300 token=VALID revision=21"];
  const con = {
    send: async () => {},
    waitFor: async <T>(match: (l: string) => T | null) => { for (const l of lines) { const r = match(l); if (r) return r; } return null; },
  };
  const seen: unknown[] = [];
  const tapped = tapUsbRevisions(con, "52e16773141a8887", (d, r) => seen.push([d, r]));
  assert.equal(await tapped.waitFor((l) => (l.startsWith("APP SET") ? l : null), 10), lines[0]);
  assert.deepEqual(seen, [["52E16773141A8887", { resource: 1, revision: 21 }]]);
});

test("Bluetooth: imzası doğrulanmış cevabın sürümü; işlem 4-5 alan", () => {
  const seen: unknown[] = [];
  onBleRevision((d, r) => seen.push([d, r]));
  bleRevision("0e8ade7e71531aa3", 1, 9);
  bleRevision("0E8ADE7E71531AA3", 5, 4);
  bleRevision("0E8ADE7E71531AA3", 2, 0);   // token yok
  onBleRevision(null);
  bleRevision("0E8ADE7E71531AA3", 1, 10);
  assert.deepEqual(seen, [["0E8ADE7E71531AA3", { resource: 1, revision: 9 }], ["0E8ADE7E71531AA3", { resource: 2, revision: 4 }]]);
});
