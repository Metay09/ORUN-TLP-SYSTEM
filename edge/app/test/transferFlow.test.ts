import assert from "node:assert/strict";
import { test } from "node:test";
import { transferStages } from "../src/features/network/transferFlow.ts";
import { operationSections, type OpTarget } from "../src/app/deviceOps.ts";

const now = 1_794_310_000_000;
const base = { now, trackerIds: ["52E16773141A8887"], collars: [],
  gateways: null, localGateway: { id: null, connected: false, held: null, capacity: null },
  edges: null, serverConfirmed: null, serverReadAt: now, label: (id: string) => id };

test("old 34 / 1780 history remains visible, and not advertised as LoRa pending", () => {
  const [tracker] = transferStages({ ...base, collars: [
    { device: "52E16773141A8887", count: 34, capacity: 1780,
      at: now - 20 * 60_000, via: "ble" as const }] });
  assert.match(tracker.entries[0].amount, /^34 \/ 1[.,]780$/);
  assert.equal(tracker.entries[0].stale, true);
  assert.match(tracker.entries[0].detail, /eski bilgi/);
  assert.doesNotMatch(tracker.entries[0].detail, /LoRa bekleyen/);
});

test("gateway capacity comes from device, stale readings do not disappear", () => {
  const gateway = { gateway: "09A462BD4B275BA5", held: 8, capacity: 1056,
    statusAt: now - 3600_000, lastSeen: now - 1000,
    lastUpload: now - 1000, via: "pc-usb" as const, trackers24h: 2 };
  const stage = transferStages({ ...base, gateways: [gateway] })[1];
  assert.equal(stage.entries.length, 1);
  assert.equal(stage.entries[0].stale, true);
  assert.match(stage.entries[0].amount, /8 \/ 1[.,]056/);
  // A live phone reading wins without counting the gateway twice.
  const direct = transferStages({ ...base, gateways: [gateway], localGateway: {
    id: gateway.gateway, connected: true, held: 1, capacity: 1056,
  } })[1];
  assert.equal(direct.entries.length, 1);
  assert.equal(direct.entries[0].stale, false);
  assert.match(direct.entries[0].amount, /1 \/ 1[.,]056/);
});

test("PC Edge pending zero is distinct from absent or stale telemetry", () => {
  const pc = { edge: "edge-a", pending: 0, delivered: 90, quarantined: 1, lastSeen: now - 3_600_000 };
  const entries = transferStages({ ...base, edges: [pc], serverConfirmed: 88 });
  assert.equal(entries[2].entries[0].amount, "0 bekliyor");
  assert.equal(entries[2].entries[0].stale, true);
  assert.equal(entries[3].entries[0].amount, "88");
  assert.equal(entries[3].entries[0].stale, false);
  const late = transferStages({ ...base, edges: [pc], serverConfirmed: 88, serverReadAt: now - 3600_000 });
  assert.equal(late[3].entries[0].stale, true);
  const absent = transferStages(base);
  assert.equal(absent[2].entries[0].amount, "—");
  assert.equal(absent[3].entries[0].amount, "—");
});

const op = (id: string, state: OpTarget["state"], timestamp: number): OpTarget => ({
  op: id, kind: "settings", device: "52E16773141A8887",
  payload: { set: { tracking: true } }, state,
  createdAt: timestamp, updatedAt: timestamp, via: "usb", note: null,
});
test("completed operations move to history without deletion, latest failure stays visible", () => {
  const r = operationSections([op("one", "applied", 1), op("two", "same", 2), op("three", "waiting", 3)]);
  assert.deepEqual(r.active.map((g) => g.op), ["three"]);
  assert.deepEqual(r.history.map((g) => g.op), ["two", "one"]);
  const rejections = operationSections([op("old", "rejected", 1), op("new", "applied", 2)]);
  assert.equal(rejections.active.length, 0);
  assert.equal(rejections.history.length, 2);
  assert.equal(operationSections([op("latest", "rejected", 4)]).active.length, 1);
});
