import assert from "node:assert/strict";
import { test } from "node:test";
import { parseEdgeQueues, parseEdgeQueueSnapshot, parseFarmGateways } from "../src/app/ownerAccountClient.ts";
import { collarStorageReadings } from "../src/app/collarStorageReadings.ts";

const sample = {
  edges: [
    { edge: "pc-a", pending: 8, delivered: 21, quarantined: 1,
      seen_utc: 1_794_300_000 },
    { edge: "pc-b", pending: 0, delivered: 11, quarantined: 0,
      seen_utc: 1_794_300_100 },
  ],
};

test("PC Edge: authenticated backend payload parses exact counts and seconds", () => {
  const rows = parseEdgeQueues(sample);
  assert.equal(rows.length, 2);
  assert.deepEqual(rows[0], {
    edge: "pc-a", pending: 8, delivered: 21, quarantined: 1,
    lastSeen: 1_794_300_000_000,
  });
  assert.equal(rows[1].pending, 0, "reported zero differs from missing telemetry");
});

test("PC Edge: reject malformed/duplicate/stale-shape responses rather than fake zero", () => {
  for (const value of [
    {}, { edges: null }, { edges: [{ ...sample.edges[0], pending: -1 }] },
    { edges: [{ ...sample.edges[0], pending: true }] },
    { edges: [{ ...sample.edges[0], edge: "bad/edge" }] },
    { edges: [{ ...sample.edges[0], seen_utc: -1 }] },
    { edges: [sample.edges[0], sample.edges[0]] },
  ]) assert.throws(() => parseEdgeQueues(value));
});

test("collar local storage reading is ephemeral, dated and never a pending count", () => {
  collarStorageReadings.clear();
  let notifications = 0;
  const unsubscribe = collarStorageReadings.subscribe(() => { notifications++; });
  const before = collarStorageReadings.snapshot();
  collarStorageReadings.note("52e16773141a8887", 17, 200, 1_794_300_000_000, "ble");
  assert.equal(collarStorageReadings.snapshot()["52E16773141A8887"].count, 17);
  assert.equal(collarStorageReadings.snapshot()["52E16773141A8887"].via, "ble");
  assert.notEqual(collarStorageReadings.snapshot(), before);
  collarStorageReadings.note("52e16773141a8887", 0, 200, 1_794_299_000_000, "usb");
  assert.equal(collarStorageReadings.snapshot()["52E16773141A8887"].count, 17,
               "a delayed reading must not override a newer reading");
  collarStorageReadings.note("bad", 99, 200, 1_794_300_100_000, "usb");
  collarStorageReadings.note("52e16773141a8887", -1, 200, 1_794_300_100_000, "usb");
  assert.equal(Object.keys(collarStorageReadings.snapshot()).length, 1);
  collarStorageReadings.clear();
  assert.equal(Object.keys(collarStorageReadings.snapshot()).length, 0);
  assert.ok(notifications >= 2);
  unsubscribe();
});

test("Gateway: last upload does not make an old memory sample fresh", () => {
  const [gateway] = parseFarmGateways({ gateways: [{
    gateway: "09A462BD4B275BA5", via: "pc-usb", held: 8, capacity: 1056,
    last_seen_utc: 1_794_300_000, last_upload_utc: 1_794_300_000,
    status_seen_utc: 1_794_292_800, trackers_24h: 2,
  }] });
  assert.equal(gateway.lastSeen, 1_794_300_000_000);
  assert.equal(gateway.statusAt, 1_794_292_800_000);
  assert.equal(gateway.held, 8);
});

test("server unique verified count is separate from PC-delivered acknowledgements", () => {
  const data = { ...sample, server_confirmed: 19 };
  const result = parseEdgeQueueSnapshot(data);
  assert.equal(result.serverConfirmed, 19);
  assert.equal(result.edges[0].delivered, 21);
  assert.equal(parseEdgeQueueSnapshot(sample).serverConfirmed, null,
               "old backend without counter must not masquerade as zero");
  assert.throws(() => parseEdgeQueueSnapshot({ ...sample, server_confirmed: -1 }));
});
