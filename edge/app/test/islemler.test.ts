// İşlemler: "Tasmaya işlendi" yalnız tasmanın onayıyla; yenisi eskisinin yerine geçer.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { LineConsole } from "../src/app/addTracker.ts";
import { deliverUsb, type DeliverDeps } from "../src/app/deliver.ts";
import { DeviceOpsBook, groupLine, groupOps, steps, type OpsServer, type OpTarget } from "../src/app/deviceOps.ts";
import { fenceDigest, polygonsOf } from "../src/app/fenceUsb.ts";
import type { LonLat, Zone } from "../src/app/zones.ts";

const A = "0E8ADE7E71538B3C", B = "0E8ADE7E71531AA3";
const ring: LonLat[] = [[28.05, 37.61], [28.06, 37.61], [28.06, 37.62], [28.05, 37.62]];
const zone: Zone = { id: "kuzey", name: "Kuzey", ring, active: true, updated: 0 };
const fencePayload = { digest: fenceDigest(polygonsOf([zone])), areas: 1, vertices: 4, zones: [{ id: "kuzey", name: "Kuzey", ring }] };

function memStore() {
  const data = new Map<string, string>();
  return { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => { data.set(k, v); } };
}

test("a new operation replaces the device's open one of the same kind", async () => {
  const book = new DeviceOpsBook(memStore());
  const op1 = await book.create("fence", [{ device: A, payload: fencePayload }, { device: B, payload: fencePayload }], null, 1000);
  await book.create("settings", [{ device: A, payload: { set: { interval_s: 900 } } }], null, 1500);
  const op2 = await book.create("fence", [{ device: A.toLowerCase(), payload: fencePayload }], null, 2000);
  const state = (op: string, d: string) => book.snapshot().find((r) => r.op === op && r.device === d)!.state;
  assert.equal(state(op1, A), "superseded");
  assert.equal(state(op1, B), "waiting");
  assert.equal(state(op2, A), "waiting");
  assert.equal(book.open(A, "fence")!.op, op2);
  assert.equal(book.open(A, "settings")!.payload && "set" in book.open(A, "settings")!.payload, true);
  await book.result(op2, A, "applied", "usb", null);
  assert.equal(book.open(A, "fence"), null);
  const g = groupOps(book.snapshot().filter((r) => r.state !== "superseded"));
  assert.deepEqual(g.map((x) => [x.kind, groupLine(x)]), [["fence", "1/1 işlendi"], ["settings", "0/1 işlendi · 1 sırada"], ["fence", "0/1 işlendi · 1 sırada"]]);
});

test("steps: USB/Bluetooth go straight to İşlendi, LoRa shows the gateway", () => {
  const t = (state: OpTarget["state"], via: OpTarget["via"]): OpTarget =>
    ({ op: "op-x", device: A, kind: "fence", payload: fencePayload, state, createdAt: 0, updatedAt: 0, via, note: null });
  assert.deepEqual(steps(t("waiting", null)).map((s) => [s.label, s.on]), [["Sırada", true], ["İşlendi", false]]);
  assert.deepEqual(steps(t("applied", "usb")).map((s) => [s.label, s.on]), [["Sırada", true], ["İşlendi", true]]);
  assert.deepEqual(steps(t("gateway", "lora")).map((s) => [s.label, s.on]), [["Sırada", true], ["Gateway'de", true], ["İşlendi", false]]);
  assert.deepEqual(steps(t("rejected", "usb")).map((s) => [s.label, s.bad ?? false]), [["Sırada", false], ["Reddedildi", true]]);
});

test("operations made offline wait on the phone and reach the account in order", async () => {
  const sent: string[] = [];
  let online = false;
  const server: OpsServer = {
    async deviceOps() { return []; },
    async createOp(_op, kind) { if (!online) throw new Error("off"); sent.push(`create ${kind}`); },
    async opResult(_op, _device, state) { if (!online) throw new Error("off"); sent.push(`result ${state}`); },
  };
  const store = memStore();
  const book = new DeviceOpsBook(store);
  const op = await book.create("fence", [{ device: A, payload: fencePayload }], server);
  await book.result(op, A, "applied", "usb", server);
  const again = new DeviceOpsBook(store);
  assert.equal(again.snapshot()[0].state, "applied");
  online = true;
  await again.refresh(server);
  assert.deepEqual(sent, ["create fence", "result applied"]);
});

/** Tracker answering settings and fence lines. */
function tracker(opts: { rejectSettings?: boolean; rejectFence?: boolean; silent?: boolean } = {}): LineConsole & { sent: string[] } {
  const out: string[] = [];
  let sa = 0, sv = 0, areas = 0, vertices = 0;
  return {
    sent: [] as string[],
    async send(line) {
      this.sent.push(line);
      if (opts.silent) return;
      if (line.startsWith("APP INTERVAL") || line.startsWith("APP SERVICES "))
        out.push(`APP SET id=1 code=${opts.rejectSettings ? "INVALID" : "APPLIED"} services=T mode=EXPLICIT token=VALID revision=2`);
      else if (line === "APP GEOFENCE?") out.push(`APP GEOFENCE id=1 resource=2 token=1 areas=${areas} vertices=${vertices} runtime=${areas ? "configured" : "clear"}`);
      else if (opts.rejectFence && line.startsWith("APP FENCE V")) out.push("APP FENCE REJECTED reason=FULL");
      else if (line === "APP FENCE BEGIN") { sa = 1; sv = 0; out.push(`APP FENCE STAGING areas=1 vertices=0`); }
      else if (line.startsWith("APP FENCE V")) { sv++; out.push(`APP FENCE STAGING areas=${sa} vertices=${sv}`); }
      else if (line === "APP FENCE COMMIT") { areas = sa; vertices = sv; out.push(`APP FENCE id=1 code=APPLIED state=CONFIGURED areas=${areas} vertices=${vertices} token=VALID`); }
    },
    async waitFor(match) { while (out.length) { const r = match(out.shift()!); if (r !== null) return r; } return null; },
  };
}

function deps(over: Partial<DeliverDeps>, log: string[]): DeliverDeps {
  const target = (op: string, kind: OpTarget["kind"], payload: OpTarget["payload"]): OpTarget =>
    ({ op, device: A, kind, payload, state: "waiting", createdAt: 0, updatedAt: 0, via: null, note: null });
  return {
    wanted: { interval_s: 900 }, has: { interval_s: 180, tracking: true, relay: false },
    openSettings: target("op-set1", "settings", { set: { interval_s: 900 } }),
    openFence: target("op-fen1", "fence", fencePayload),
    reportSettings: async (v) => { log.push("report " + JSON.stringify(v)); },
    recordFence: async (sum) => { log.push("fence " + sum.areas); },
    result: async (op, state, note) => { log.push(`${op} ${state}${note ? " " + note : ""}`); },
    ...over,
  };
}

test("on USB: settings then zones, each marked only on the tracker's answer", async () => {
  const log: string[] = [];
  const t = tracker();
  const r = await deliverUsb(t, deps({}, log));
  assert.deepEqual(r, { wrote: ["ayarlar", "alanlar (1)"], problems: [] });
  assert.deepEqual(log, ['report {"interval_s":900}', "op-set1 applied", "fence 1", "op-fen1 applied"]);
  assert.equal(t.sent[0], "APP INTERVAL 900");
  // Nothing to change: the open settings operation is "same", nothing is written.
  const log2: string[] = [];
  await deliverUsb(tracker(), deps({ wanted: { interval_s: 180 }, openFence: null }, log2));
  assert.deepEqual(log2, ["op-set1 same"]);
});

test("a refusal is 'rejected'; silence leaves the operation in the queue", async () => {
  const log: string[] = [];
  const r = await deliverUsb(tracker({ rejectSettings: true, rejectFence: true }), deps({}, log));
  assert.equal(r.wrote.length, 0);
  assert.equal(log.filter((l) => l.includes("rejected")).length, 2);
  const quiet: string[] = [];
  const r2 = await deliverUsb(tracker({ silent: true }), deps({}, quiet));
  assert.deepEqual(quiet, []);          // no answer: nothing marked, both stay "Sırada"
  assert.equal(r2.problems.length, 2);
});

test("a tracker already queued with the same zones is not asked again", async () => {
  const { toApply } = await import("../src/app/fencePlan.ts");
  const plan = (device: string, state: "stale" | "never" | "current", digest: string) =>
    ({ device, name: device, zones: [zone], errors: [], digest, record: null, state });
  const plans = [plan(A, "never", "d1"), plan(B, "stale", "d2"), plan("0E8ADE7E7153FFFF", "current", "d3")];
  assert.deepEqual(toApply(plans).map((p) => p.device), [A, B]);
  assert.deepEqual(toApply(plans, new Map([[A, "d1"], [B, "old"]])).map((p) => p.device), [B]);
  assert.deepEqual(toApply(plans, new Map(), B).map((p) => p.device), [B]);
});
