// Step 3A / D1: the phone opens records sealed with a REAL Tracker credential
// using only the per-epoch read key from the backend.
import assert from "node:assert/strict";
import { test } from "node:test";
import { concat, fromHex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { KeyRing } from "../src/core/keys.ts";
import { Ledger } from "../src/core/ledger.ts";
import { benchCredentials, openProductFrame } from "../src/core/product.ts";
import { SCHEMA } from "../src/core/db.ts";
import { fetchReadKeys } from "../src/core/sync.ts";
import { KAT_PERIODIC, seal } from "./fixtures.ts";
import { NodeDb } from "./nodeDb.ts";

// backend/orun_credentials.py vector: credential_id 10..1F, root 40..5F,
// epoch 1 -> traffic key (the same vector pins the firmware grant test).
const CRED = new Uint8Array(16).map((_, i) => 0x10 + i);
const ROOT = new Uint8Array(32).map((_, i) => 0x40 + i);
const TRAFFIC = "A432F382698064DB3C7518DF3848C5EF";

function realFrame(counter: number): Uint8Array {
  const kat = KAT_PERIODIC();
  const plain = openProductFrame(kat, benchCredentials).plaintext.slice();
  new DataView(plain.buffer).setUint32(12, 5000 + counter);
  return seal(kat, plain, counter, CRED, ROOT, 1);
}

test("read key opens real-credential records; the bench key does not", async () => {
  const db = new NodeDb();
  for (const s of SCHEMA) await db.execute(s);
  const frame = realFrame(1);
  assert.equal((await new Ledger(db, benchCredentials).ingest(frame)).state, "REJECTED");
  const ring = new KeyRing();
  const ledger = new Ledger(db, ring.lookup);
  assert.equal((await ledger.ingest(frame)).state, "REJECTED");      // no key yet
  assert.equal(await ring.store(db, [
    { device: "1122334455667788", epoch: 1, trafficKey: TRAFFIC },
    { device: "zz", epoch: 1, trafficKey: TRAFFIC },                  // ignored
    { device: "1122334455667788", epoch: 2, trafficKey: "00" },       // ignored
  ]), 1);
  assert.equal(await ring.store(db, [{ device: "1122334455667788", epoch: 1, trafficKey: TRAFFIC }]), 0);
  assert.equal((await ledger.ingest(frame)).state, "STORED");
  // A wrong key never opens it.
  const wrongDb = new NodeDb();
  for (const s of SCHEMA) await wrongDb.execute(s);
  const wrong = new KeyRing(false);
  await wrong.store(wrongDb, [{ device: "1122334455667788", epoch: 1, trafficKey: TRAFFIC.replace(/^A/, "B") }]);
  assert.equal((await new Ledger(wrongDb, wrong.lookup).ingest(frame)).state, "REJECTED");
  // Persisted: a fresh ring loaded from the database opens it too.
  const again = new KeyRing(false);
  await again.load(db);
  assert.equal(again.size, 1);
  assert.equal(openProductFrame(realFrame(2), again.lookup).sequence, 5002);
  // The bench path still opens old test-key records.
  assert.equal(openProductFrame(KAT_PERIODIC(), ring.lookup).sequence, 257);
  assert.throws(() => openProductFrame(KAT_PERIODIC(), again.lookup));   // bench off
});

test("BLE ingest with the key ring: unopenable records are not accepted", async () => {
  const db = new NodeDb();
  const ring = new KeyRing();
  const core = new EdgeCore(db, { credentials: ring.lookup, clock: () => 1_800_000_000 });
  await core.init();
  await core.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00010420")));
  const frame = realFrame(3);
  const h = new Uint8Array(18); const v = new DataView(h.buffer);
  h[0] = 2; v.setUint16(1, 0); v.setUint16(3, 4); v.setBigUint64(5, 1n); h[17] = frame.length;
  const msg = concat(h, frame);
  assert.match((await core.handleMessage(msg))!, /^RED/);
  assert.equal(core.nextAcceptMessage(), null);          // Gateway keeps it
  await core.exclusive(() => ring.store(db, [{ device: "1122334455667788", epoch: 1, trafficKey: TRAFFIC }]));
  assert.match((await core.handleMessage(msg))!, /^YENİ/);
  assert.notEqual(core.nextAcceptMessage(), null);       // now handed off
});

test("fetchReadKeys: answers and failures", async () => {
  const ok = await fetchReadKeys(async (url, token) => {
    assert.equal(url, "http://x/v1/edge/keys"); assert.equal(token, "t");
    return { status: 200, body: { keys: [{ device: "1122334455667788", epoch: 1, traffic_key: TRAFFIC }] } };
  }, "http://x/", "t");
  assert.deepEqual(ok, [{ device: "1122334455667788", epoch: 1, trafficKey: TRAFFIC }]);
  await assert.rejects(fetchReadKeys(async () => ({ status: 401, body: {} }), "http://x", "t"), /token/);
  await assert.rejects(fetchReadKeys(async () => ({ status: 200, body: {} }), "http://x", "t"), /anlaşılamadı/);
});
