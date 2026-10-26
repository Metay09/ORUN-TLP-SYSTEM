// SF5L8 phone -> backend upload, host only (fake backend here; the real
// Python backend is exercised by firmware/tests/sf5/test_sf5l8_backend_sync.py).
import assert from "node:assert/strict";
import { test } from "node:test";
import { concat, fromHex, hex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { benchCredentials, openProductFrame } from "../src/core/product.ts";
import { BATCH, Uploader, type Post } from "../src/core/sync.ts";
import { track } from "./fixtures.ts";
import { NodeDb } from "./nodeDb.ts";

function held(page: number, slot: number, obj: Uint8Array, link: [number, number] | null) {
  const h = new Uint8Array(18); const v = new DataView(h.buffer);
  h[0] = 2; v.setUint16(1, page); v.setUint16(3, slot); v.setBigUint64(5, 1n);
  if (link) { h[13] = 1; v.setInt16(14, link[0]); v.setInt8(16, link[1]); }
  h[17] = obj.length; return concat(h, obj);
}

async function phoneWith(n: number) {
  let now = 1_800_000_000;
  const core = new EdgeCore(new NodeDb(), { credentials: benchCredentials, clock: () => now });
  await core.init();
  await core.sessionAlive("ble");
  await core.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00000420")));
  const frames = track(n);
  for (let i = 0; i < n; i++) { now += 1; await core.handleMessage(held(0, i, frames[i], i % 2 ? [-80, 5] : null)); }
  return { core, frames, db: (core as unknown as { db: NodeDb }).db, tick: (s: number) => { now += s; } };
}

test("upload: newest first, exact frames + local facts, final answers marked", async () => {
  const { core, frames, db } = await phoneWith(5);
  const bodies: any[] = [];
  const post: Post = async (url, token, body) => {
    assert.equal(url, "http://gw.test:8090/v1/edge/batch");
    assert.equal(token, "ORUN-BENCH");
    const b = JSON.parse(body); bodies.push(b);
    const words = ["STORED", "DUPLICATE", "UNCERTAIN", "CONFLICT", "REJECTED"];
    return { status: 200, body: { results: b.records.map((_: unknown, i: number) => words[i]), sessions_ok: true } };
  };
  const up = new Uploader(db, post, (fn) => core.exclusive(fn));
  const r = await up.runOnce("http://gw.test:8090/", "ORUN-BENCH");
  assert.deepEqual(r, { sent: 5, final: 4, uncertain: 1 });
  const b = bodies[0];
  assert.match(b.edge, /^phone-[0-9a-f]{12}$/);
  assert.deepEqual(b.records.map((x: any) => x.frame), [...frames].reverse().map(hex));   // newest first
  assert.equal(b.records[0].gateway, "09A462BD4B275BA5");
  assert.equal(b.records[1].rssi, -80);                     // frame 3 had a link
  assert.equal(b.records[0].rssi, null);
  assert.equal(typeof b.records[0].first_seen_utc, "number");
  assert.equal(b.sessions.length, 1);
  assert.equal(b.sessions[0].transport, "ble");
  assert.deepEqual(await up.counts(), { uploaded: 2, pending: 1, conflict: 1, rejected: 1 });
  // Only the UNCERTAIN one is sent again; an unchanged session is not resent.
  const again = await up.runOnce("http://gw.test:8090", "ORUN-BENCH");
  assert.equal(again.sent, 1);
  assert.equal(bodies[1].records[0].frame, hex(frames[2]));
  assert.equal(bodies[1].sessions.length, 1);   // first answer marked it; same content -> still sent with a record
  assert.equal((await up.runOnce("http://gw.test:8090", "ORUN-BENCH")).sent, 0);
  assert.equal(await up.edgeId(), b.edge);       // stable
});

test("upload: network, HTTP and protocol failures mark nothing", async () => {
  const { core, db } = await phoneWith(3);
  const replies: (() => Promise<any>)[] = [
    async () => { throw new Error("offline"); },
    async () => ({ status: 401, body: {} }),
    async () => ({ status: 500, body: {} }),
    async () => ({ status: 200, body: { results: ["STORED"] } }),   // wrong length
  ];
  let i = 0;
  const up = new Uploader(db, () => replies[i++](), (fn) => core.exclusive(fn));
  await assert.rejects(up.runOnce("http://x", "t"), /offline/);
  await assert.rejects(up.runOnce("http://x", "t"), /token/);
  await assert.rejects(up.runOnce("http://x", "t"), /HTTP 500/);
  await assert.rejects(up.runOnce("http://x", "t"), /anlaşılamadı/);
  assert.equal((await up.counts()).pending, 3);
});

test("upload: drain sends batches of 100 until done", async () => {
  const { core, db } = await phoneWith(BATCH + 30);
  let calls = 0;
  const up = new Uploader(db, async (_u, _t, body) => {
    calls++;
    return { status: 200, body: { results: JSON.parse(body).records.map(() => "STORED"), sessions_ok: true } };
  }, (fn) => core.exclusive(fn));
  const r = await up.drain("http://x", "t");
  assert.equal(r.final, BATCH + 30);
  assert.equal(calls, 2);
  assert.equal((await up.counts()).uploaded, BATCH + 30);
  assert.equal(openProductFrame(track(1)[0], benchCredentials).family, 1);
});

test("upload: all-UNCERTAIN newest page does not starve older pending records", async () => {
  const { core, db } = await phoneWith(BATCH + 30);
  const attempted: string[] = [];
  let calls = 0;
  const up = new Uploader(db, async (_url, _token, body) => {
    calls++;
    const records = (JSON.parse(body) as { records: { frame: string }[] }).records;
    attempted.push(...records.map((r) => r.frame));
    return { status: 200, body: {
      results: records.map(() => calls === 1 ? "UNCERTAIN" : "STORED"),
      sessions_ok: true,
    } };
  }, (fn) => core.exclusive(fn));

  const first = await up.drain("http://x", "t");
  assert.deepEqual(first, { sent: BATCH + 30, final: 30, uncertain: BATCH });
  assert.equal(calls, 2);                      // skipped stalled front, still sent the older 30
  assert.equal(new Set(attempted).size, BATCH + 30); // no duplicate in a drain
  assert.deepEqual(await up.counts(), { uploaded: 30, pending: BATCH, conflict: 0, rejected: 0 });

  const second = await up.drain("http://x", "t");
  assert.deepEqual(second, { sent: BATCH, final: BATCH, uncertain: 0 });
  assert.equal((await up.counts()).pending, 0);
});

test("upload: mixed UNKNOWN answers do not cause retry storms within one drain", async () => {
  const { core, db } = await phoneWith(BATCH + 17);
  let calls = 0;
  const framesSent: string[] = [];
  const up = new Uploader(db, async (_url, _token, body) => {
    calls++;
    const rows = (JSON.parse(body) as { records: { frame: string }[] }).records;
    framesSent.push(...rows.map((r) => r.frame));
    return { status: 200, body: {
      results: rows.map((_r, i) => calls === 1 && i !== 0 ? "UNCERTAIN" : "STORED"),
      sessions_ok: true,
    } };
  }, (fn) => core.exclusive(fn));
  const result = await up.drain("http://x", "t");
  assert.deepEqual(result, { sent: BATCH + 17, final: 18, uncertain: BATCH - 1 });
  assert.equal(calls, 2);
  assert.equal(framesSent.length, new Set(framesSent).size);
  assert.equal((await up.counts()).pending, BATCH - 1);
});

test("ping: empty batch checks address and token", async () => {
  const { core, db } = await phoneWith(1);
  const up = new Uploader(db, async (_u, token, body) => {
    const b = JSON.parse(body);
    assert.deepEqual([b.records, b.sessions], [[], []]);
    return token === "good" ? { status: 200, body: { results: [], records: 7 } } : { status: 401, body: {} };
  }, (fn) => core.exclusive(fn));
  assert.equal(await up.ping("http://x/", "good"), 7);
  await assert.rejects(up.ping("http://x", "bad"), /token/);
});
