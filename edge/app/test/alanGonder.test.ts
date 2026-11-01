// 3a: alanları USB ile tasmaya gönder.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { LineConsole } from "../src/app/addTracker.ts";
import { decimalE7, fenceCommands, fenceDigest, polygonsOf, writeFence } from "../src/app/fenceUsb.ts";
import { needsSending, trackerFencePlans } from "../src/app/fencePlan.ts";
import { fenceState, TrackerFenceBook, type FenceRecord, type FenceServer } from "../src/app/trackerFences.ts";
import type { TargetBook } from "../src/app/zoneTargets.ts";
import type { LonLat, Zone } from "../src/app/zones.ts";

const zone = (id: string, ring: LonLat[]): Zone => ({ id, name: id, ring, active: true, updated: 0 });
const square = zone("kuzey", [[28.05,37.61],[28.06,37.61],[28.06,37.62],[28.05,37.62],[28.05,37.61]]);
const tri = zone("dere", [[28.1234567,37.7000001],[28.13,37.7],[28.125,37.71]]);
const A = "00000000000000AA", B = "00000000000000BB";

/** Tracker firmware as seen over USB: stages, commits, reports. */
class FakeTracker implements LineConsole {
  sent: string[] = [];
  private out: string[] = [];
  areas = 0; vertices = 0; private sa = 0; private sv = 0;
  private opts: { rejectOn?: string; dropVertex?: boolean; commitAreas?: number; reportAreas?: number };
  constructor(opts: FakeTracker["opts"] = {}) { this.opts = opts; }
  async send(line: string): Promise<void> {
    this.sent.push(line);
    if (this.opts.rejectOn && line.startsWith(this.opts.rejectOn)) { this.out.push("APP FENCE REJECTED reason=bad"); return; }
    const staged = () => this.out.push(`APP FENCE STAGING areas=${this.sa} vertices=${this.sv}`);
    if (line === "APP GEOFENCE?") {
      const n = this.opts.reportAreas ?? this.areas;
      this.out.push("APP NOISE", `APP GEOFENCE id=7 resource=1 token=2 areas=${n} vertices=${this.vertices} runtime=${this.areas ? "configured" : "unconfigured"}`);
    } else if (line === "APP FENCE BEGIN") { this.sa = 1; this.sv = 0; staged(); }
    else if (line.startsWith("APP FENCE V ")) { if (!this.opts.dropVertex || this.sv !== 2) this.sv += 1; staged(); }
    else if (line === "APP FENCE AREA") { this.sa += 1; staged(); }
    else if (line === "APP FENCE COMMIT") {
      this.areas = this.sa; this.vertices = this.sv;
      this.out.push(`APP FENCE id=7 code=APPLIED state=CONFIGURED areas=${this.opts.commitAreas ?? this.areas} vertices=${this.vertices} token=VALID`);
    } else if (line === "APP FENCE ABORT") this.out.push("APP FENCE ABORTED");
  }
  async waitFor<T>(match: (line: string) => T | null): Promise<T | null> {
    while (this.out.length) { const r = match(this.out.shift()!); if (r) return r; }
    return null;   // no answer within the time limit
  }
}

test("coordinates are written as exact E7 decimals", () => {
  assert.equal(decimalE7(376107000), "37.6107000");
  assert.equal(decimalE7(280500000), "28.0500000");
  assert.equal(decimalE7(-5), "-0.0000005");
  assert.equal(decimalE7(-376107001), "-37.6107001");
});

test("commands match tools/orun_fence_usb.py for the same zones", () => {
  // python3 -c 'commands(parse_geojson(<the same two polygons>))'
  const python = [["APP FENCE BEGIN",1,0],["APP FENCE V 37.6100000 28.0500000",1,1],["APP FENCE V 37.6100000 28.0600000",1,2],
    ["APP FENCE V 37.6200000 28.0600000",1,3],["APP FENCE V 37.6200000 28.0500000",1,4],["APP FENCE AREA",2,4],
    ["APP FENCE V 37.7000001 28.1234567",2,5],["APP FENCE V 37.7000000 28.1300000",2,6],["APP FENCE V 37.7100000 28.1250000",2,7]];
  assert.deepEqual(fenceCommands(polygonsOf([square, tri])).map((c) => [c.line, c.area, c.vertices]), python);
});

test("digest follows content and order, not names", () => {
  const d = fenceDigest(polygonsOf([square, tri]));
  assert.match(d, /^[0-9a-f]{16}$/);
  assert.equal(fenceDigest(polygonsOf([{ ...square, name: "başka" }, tri])), d);
  assert.notEqual(fenceDigest(polygonsOf([tri, square])), d);
});

test("a successful write is accepted only after commit and read-back", async () => {
  const t = new FakeTracker();
  const steps: number[] = [];
  const sum = await writeFence(t, [square, tri], (done, total) => steps.push(done / total));
  assert.deepEqual([sum.areas, sum.vertices], [2, 7]);
  assert.equal(sum.digest, fenceDigest(polygonsOf([square, tri])));
  assert.deepEqual(t.sent.slice(-2), ["APP FENCE COMMIT", "APP GEOFENCE?"]);
  assert.equal(t.sent[0], "APP GEOFENCE?");
  assert.equal(steps.at(-1), 1);
  assert.ok(!t.sent.includes("APP FENCE ABORT"));
});

test("a rejected or miscounted staging is aborted and never committed", async () => {
  for (const t of [new FakeTracker({ rejectOn: "APP FENCE AREA" }), new FakeTracker({ dropVertex: true })]) {
    await assert.rejects(writeFence(t, [square, tri]), /reddetti|onay gelmedi/);
    assert.equal(t.sent.at(-1), "APP FENCE ABORT");
    assert.ok(!t.sent.includes("APP FENCE COMMIT"));
  }
});

test("a commit or read-back that does not match fails (nothing recorded)", async () => {
  await assert.rejects(writeFence(new FakeTracker({ commitAreas: 1 }), [square, tri]), /doğrulamadı/);
  await assert.rejects(writeFence(new FakeTracker({ reportAreas: 1 }), [square, tri]), /tutmuyor/);
});

test("limits are checked before anything is sent", async () => {
  const t = new FakeTracker();
  const many = Array.from({ length: 9 }, (_, i) => zone(`z${i}`, square.ring));
  await assert.rejects(writeFence(t, many), /en fazla 8 alan/);
  await assert.rejects(writeFence(t, []), /alan yok/);
  assert.deepEqual(t.sent, []);
});

test("fence state: none, never, current, stale", () => {
  const rec: FenceRecord = { digest: "aa", areas: 1, vertices: 4, zones: [], appliedAt: 1 };
  assert.equal(fenceState(null, null), "none");
  assert.equal(fenceState(rec, null), "none");
  assert.equal(fenceState(null, "aa"), "never");
  assert.equal(fenceState(rec, "aa"), "current");
  assert.equal(fenceState(rec, "bb"), "stale");
});

function memStore(): Pick<Storage, "getItem" | "setItem"> & { data: Map<string, string> } {
  const data = new Map<string, string>();
  return { data, getItem: (k) => data.get(k) ?? null, setItem: (k, v) => { data.set(k, v); } };
}

test("the record book keeps unsent records until the account takes them", async () => {
  const store = memStore();
  const rec: FenceRecord = { digest: "0123456789abcdef", areas: 1, vertices: 4, zones: [{ id: "kuzey", name: "Kuzey" }], appliedAt: 5 };
  const remote: Record<string, FenceRecord> = {};
  let online = false;
  const server: FenceServer = {
    async trackerFences() { if (!online) throw new Error("off"); return { ...remote }; },
    async recordFence(dev, r) { if (!online) throw new Error("off"); remote[dev] = { ...r, appliedAt: 9 }; },
  };
  const book = new TrackerFenceBook(store);
  await book.set(A.toLowerCase(), rec, server);           // offline: stays here
  assert.equal(book.get(A)?.digest, rec.digest);
  assert.deepEqual(remote, {});
  const again = new TrackerFenceBook(store);              // survives a restart
  assert.equal(again.get(A)?.areas, 1);
  online = true;
  remote[B] = { ...rec, digest: "fedcba9876543210" };
  await again.refresh(server);
  assert.equal(remote[A]?.digest, rec.digest);            // pushed
  assert.equal(again.get(B)?.digest, "fedcba9876543210"); // pulled
  assert.ok(!JSON.parse(store.data.get("orun.trackerFences.v1")!).pending[A]);
});

test("broken saved records are dropped", () => {
  const store = memStore();
  store.setItem("orun.trackerFences.v1", JSON.stringify({ records: { [A]: { digest: 1 }, bad: { digest: "x" } }, pending: "?" }));
  assert.deepEqual(new TrackerFenceBook(store).snapshot(), {});
});

test("each tracker's state comes from its own target zones", () => {
  const book: TargetBook = { groups: [], rules: {
    kuzey: { scope: "all", devices: [], groups: [] },
    dere: { scope: "selected", devices: [A], groups: [] },
  } };
  const trackers = [{ device: A, name: "Sarıkız" }, { device: B, name: "Benekli" }, { device: "00000000000000CC", name: "Yok" }];
  const onlyAll: TargetBook = { ...book, rules: { dere: book.rules.dere } };
  const sentToB = fenceDigest(polygonsOf([square]));
  const plans = trackerFencePlans(book, [square, tri], trackers.slice(0, 2),
    { [B]: { digest: sentToB, areas: 1, vertices: 4, zones: [{ id: "kuzey", name: "kuzey" }], appliedAt: 1 } });
  assert.deepEqual(plans.map((p) => p.zones.map((z) => z.id)), [["kuzey", "dere"], ["kuzey"]]);
  assert.deepEqual(plans.map((p) => p.state), ["never", "current"]);
  assert.deepEqual(needsSending(plans).map((p) => p.name), ["Sarıkız"]);
  // The zone changes: B's copy is now old.
  const moved = { ...square, ring: square.ring.map(([lo, la]) => [lo + 0.001, la] as LonLat) };
  assert.equal(trackerFencePlans(book, [moved, tri], trackers.slice(0, 2), { [B]: plans[1].record! })[1].state, "stale");
  // A tracker no zone targets gets nothing.
  assert.equal(trackerFencePlans(onlyAll, [square, tri], trackers, {})[2].state, "none");
});
