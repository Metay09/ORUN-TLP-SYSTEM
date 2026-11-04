// 4c-1b: Bluetooth'tan ayar yazma — hesaptaki telefonun yazma anahtarı, tasmanın
// tek kullanımlık sorusu, iki yönde imza. Vektör firmware
// (firmware/tests/m7/test_ble_write.cpp) ve sunucuyla
// (backend/tests/test_gateway_listesi.py) aynı.
import assert from "node:assert/strict";
import { test } from "node:test";
import { concat, fromHex, hex } from "../src/core/bytes.ts";
import { hmacSha256 } from "../src/core/crypto.ts";
import { AppReassembler, ERROR_TYPE, requestFrame, requestFrames, type AppMessage } from "../src/app/bleAppFrames.ts";
import {
  bleSettingWriter, bleWrite, bleWriteFence, NoWriteKey, OP, parseChallenge, parseResult, servicesValue, STAGE,
  stagedDigest, WRITE, WriteRefused, writeRequest, type Exchange,
} from "../src/app/bleWrite.ts";
import { deliverBle, deliverSettings } from "../src/app/deliver.ts";
import type { E7, Zone } from "../src/app/zones.ts";
import { DeviceRejected } from "../src/app/fenceUsb.ts";
import { writeWaiting } from "../src/app/trackerSettings.ts";
import { WriteKeyBook } from "../src/app/writeKeys.ts";

const DEV = "0E8ADE7E71531AA3";
const KEY = fromHex("75BF1BD3052DFA4CD5E9DB9DD084B98DB9622C2FABE44588FB58E02EC0952DD2");
const CH = Uint8Array.from({ length: 16 }, (_, i) => 0xc0 + i);
const enc = new TextEncoder();
const le32 = (n: number) => Uint8Array.from([n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]);

function challengeMsg(device = DEV, epoch = 1, ch: Uint8Array = CH): AppMessage {
  return { type: WRITE.challengeAnswer, correlation: 1, payload: concat(Uint8Array.of(0), fromHex(device), le32(epoch), ch) };
}

/** What the tasma signs (independent of bleWrite.ts). */
function sign(key: Uint8Array, domain: string, device: string, epoch: number, ch: Uint8Array, body: Uint8Array): Uint8Array {
  const be = Uint8Array.from([(epoch >>> 24) & 0xff, (epoch >>> 16) & 0xff, (epoch >>> 8) & 0xff, epoch & 0xff]);
  return hmacSha256(key, concat(enc.encode(domain), fromHex(device), be, ch, body)).slice(0, 16);
}

test("the shared vector: request tag, signed answer, tampering", () => {
  const c = parseChallenge(challengeMsg());
  assert.deepEqual([c.device, c.epoch, hex(c.challenge)], [DEV, 1, hex(CH)]);
  const req = writeRequest(KEY, c, OP.interval, 180);
  assert.equal(hex(req), "01B4000000" + "79D0DB48C72ED2FF66C18AA77177D44A");
  const body = fromHex("0001B40000001E00010102000000");
  const answer: AppMessage = { type: WRITE.writeAnswer, correlation: 2, payload: concat(body, fromHex("4CD68F40DDA649FBF6F160E046FC09C6")) };
  assert.deepEqual(parseResult(KEY, c, answer, OP.interval), {
    outcome: "APPLIED", op: 1, intervalS: 180, bleWindowS: 30, serviceMode: 1, services: 1, revision: 2,
  });
  for (const i of [0, 3, 13, 14, 29]) {
    const bad = answer.payload.slice(); bad[i] ^= 1;
    assert.throws(() => parseResult(KEY, c, { ...answer, payload: bad }, OP.interval), /doğrulanamadı|beklenmedik/);
  }
  assert.throws(() => parseResult(KEY, c, answer, OP.bleWindow), /beklenmedik/);          // answer to another op
  assert.throws(() => parseResult(fromHex("00".repeat(32)), c, answer, OP.interval), /doğrulanamadı/);
  assert.throws(() => parseChallenge({ type: ERROR_TYPE, correlation: 1, payload: Uint8Array.of(3, 0x10) }), WriteRefused);
  assert.throws(() => parseChallenge({ ...challengeMsg(), payload: challengeMsg().payload.slice(0, 28) }), /beklenmedik/);
  assert.equal(servicesValue({ tracking: true, relay: false }), 0x0101);
  assert.equal(servicesValue({ tracking: true, relay: true }), 0x0301);
  assert.equal(servicesValue({ tracking: false, relay: false }), 0x0001);
});

test("a write with data goes out in 12-byte fragments", () => {
  const p = Uint8Array.from({ length: 21 }, (_, i) => i);
  const fs = requestFrames(WRITE.write, 0x1234, p);
  assert.equal(fs.length, 2);
  assert.deepEqual([...fs[0].subarray(0, 8)], [1, 0x11, 0x01, 0, 0x34, 0x12, 21, 0]);
  assert.deepEqual([...fs[1].subarray(0, 8)], [1, 0x11, 0x02, 1, 0x34, 0x12, 21, 0]);
  assert.deepEqual([...fs[0].subarray(8), ...fs[1].subarray(8)], [...p]);
  assert.deepEqual(requestFrames(WRITE.challenge, 7, new Uint8Array(0)), [requestFrame(WRITE.challenge, 7)]);
  assert.throws(() => requestFrames(1, 1, new Uint8Array(49)));
  // The tasma's 3-fragment challenge answer reassembles like any answer.
  const r = new AppReassembler();
  const payload = challengeMsg().payload;
  let got: AppMessage | null = null;
  for (let at = 0, k = 0; at < payload.length; at += 12, k++) {
    const chunk = payload.subarray(at, at + 12);
    got = r.push(Uint8Array.from([1, 0x90, (k === 0 ? 1 : 0) | (at + 12 >= payload.length ? 2 : 0), k, 9, 0, payload.length, 0, ...chunk]));
  }
  assert.equal(parseChallenge(got!).device, DEV);
});

/** A tasma that does what firmware/src/ble_write_service.cpp does. */
class FakeTracker {
  config = { interval: 180, ble: 30, mode: 0, services: 0, revision: 1 };
  key = KEY;
  device = DEV;
  epoch = 1;
  next = 0x10;
  challenge: Uint8Array | null = null;
  refuse: number | null = null;   // ERROR code answered to the write
  forge = false;                  // sign the answer with a wrong key
  writes: number[] = [];

  staged: E7[][] | null = null;
  fence = { state: 1, areas: 0, vertices: 0, revision: 1 };
  stageSteps = 0;

  private stage(payload: Uint8Array): AppMessage {
    this.stageSteps++;
    let r = 0;
    const action = payload[0];
    if (action === STAGE.begin) this.staged = [[]];
    else if (action === STAGE.abort) this.staged = null;
    else if (!this.staged) r = 1;
    else if (action === STAGE.nextArea) { if (!this.staged[this.staged.length - 1].length) r = 3; else this.staged.push([]); }
    else if (action === STAGE.vertices) {
      const n = payload[1];
      if (n < 1 || n > 5 || payload.length !== 2 + 8 * n) r = 5;
      else for (let i = 0; i < n; i++) {
        const v = new DataView(payload.buffer, payload.byteOffset + 2 + 8 * i, 8);
        this.staged[this.staged.length - 1].push([v.getInt32(0, true), v.getInt32(4, true)]);
      }
    } else r = 5;
    const areas = this.staged ? this.staged.filter((a) => a.length).length : 0;
    const vertices = this.staged ? this.staged.reduce((n, a) => n + a.length, 0) : 0;
    return { type: WRITE.stageAnswer, correlation: 3, payload: Uint8Array.of(r, areas, vertices & 0xff, vertices >> 8) };
  }

  readonly exchange: Exchange = async (type, payload) => {
    if (type === WRITE.stage) return this.stage(payload);
    if (type === WRITE.challenge) {
      this.challenge = Uint8Array.from({ length: 16 }, (_, i) => (this.next + i) & 0xff);
      this.next += 0x10;
      return challengeMsg(this.device, this.epoch, this.challenge);
    }
    assert.equal(type, WRITE.write);
    const ch = this.challenge; this.challenge = null;                    // single use
    if (this.refuse !== null) return { type: ERROR_TYPE, correlation: 2, payload: Uint8Array.of(this.refuse, type) };
    const isFence = payload[0] === OP.fence;
    const staged = (this.staged ?? []).filter((x) => x.length);
    const macBody = isFence ? concat(payload.subarray(0, 5), stagedDigest(staged)) : payload.subarray(0, 5);
    if (!ch || payload.length !== 21 || hex(sign(this.key, "ORUN-BLE-WRITE-REQ-v1", this.device, this.epoch, ch, macBody)) !== hex(payload.subarray(5)))
      return { type: ERROR_TYPE, correlation: 2, payload: Uint8Array.of(3, type) };
    const op = payload[0], v = payload[1] | (payload[2] << 8) | (payload[3] << 16) | (payload[4] << 24);
    if (isFence) {
      const vertices = staged.reduce((n, a) => n + a.length, 0);
      const ok = staged.length > 0 && v === (staged.length | (vertices << 8)) && staged.every((a) => a.length >= 3);
      this.staged = null;
      if (ok) this.fence = { state: 2, areas: staged.length, vertices, revision: this.fence.revision + 1 };
      const f = this.fence;
      const body = concat(Uint8Array.of(ok ? 0 : 2, op, f.state, f.areas, f.vertices & 0xff, f.vertices >> 8), new Uint8Array(4), le32(f.revision));
      return { type: WRITE.writeAnswer, correlation: 2,
        payload: concat(body, sign(this.forge ? fromHex("11".repeat(32)) : this.key, "ORUN-BLE-WRITE-RES-v1", this.device, this.epoch, ch, body)) };
    }
    this.writes.push(op);
    let outcome = 0;
    const before = { ...this.config };
    if (op === 1) { if (v < 60) outcome = 2; else this.config.interval = v; }
    else if (op === 3) { if (v > 300) outcome = 2; else this.config.ble = v; }
    else if (op === 2) { if ((v >> 8) & 0x04) outcome = 2; else { this.config.mode = v & 0xff; this.config.services = (v >> 8) & 0xff; } }
    else outcome = 2;
    if (outcome === 0 && JSON.stringify(before) === JSON.stringify(this.config)) outcome = 1;
    if (outcome === 0) this.config.revision++;
    const c = this.config;
    const body = concat(Uint8Array.of(outcome, op), le32(c.interval), Uint8Array.of(c.ble & 0xff, c.ble >> 8, c.mode, c.services), le32(c.revision));
    return { type: WRITE.writeAnswer, correlation: 2,
      payload: concat(body, sign(this.forge ? fromHex("11".repeat(32)) : this.key, "ORUN-BLE-WRITE-RES-v1", this.device, this.epoch, ch, body)) };
  };
}

const keyFor = (device: string, epoch: number) => (device === DEV && epoch === 1 ? KEY : null);

test("waiting settings are written over Bluetooth in the USB order", async () => {
  const t = new FakeTracker();
  const done = await writeWaiting(bleSettingWriter(t.exchange, keyFor, DEV.toLowerCase()),
    { interval_s: 900, ble_window_s: 60, tracking: true, relay: false },
    { interval_s: 180, ble_window_s: 30 });
  assert.deepEqual(done, { interval_s: 900, ble_window_s: 60, tracking: true, relay: false });
  assert.deepEqual(t.writes, [1, 3, 2]);
  assert.deepEqual(t.config, { interval: 900, ble: 60, mode: 1, services: 1, revision: 4 });
  // Already there: UNCHANGED is fine; nothing waiting writes nothing.
  assert.equal(await bleSettingWriter(t.exchange, keyFor, DEV).interval(900), "unchanged");
  assert.deepEqual(await writeWaiting(bleSettingWriter(t.exchange, keyFor, DEV), { interval_s: 900 }, { interval_s: 900 }), {});
});

test("refusals: no key, another device, tasma says no, forged answer", async () => {
  const t = new FakeTracker();
  t.epoch = 2;                                                     // re-added tasma, old key
  await assert.rejects(bleWrite(t.exchange, keyFor, DEV, OP.interval, 600), NoWriteKey);
  t.epoch = 1;
  await assert.rejects(bleWrite(t.exchange, keyFor, "52E16773141A8887", OP.interval, 600), /beklenen tasma değil/);
  t.refuse = 2;
  await assert.rejects(bleWrite(t.exchange, keyFor, DEV, OP.interval, 600), /meşgul/);
  t.refuse = 3;
  await assert.rejects(bleWrite(t.exchange, keyFor, DEV, OP.interval, 600), WriteRefused);
  t.refuse = null;
  t.key = fromHex("22".repeat(32));                               // the tasma does not know this phone's key
  await assert.rejects(bleWrite(t.exchange, keyFor, DEV, OP.interval, 600), /kabul etmedi/);
  t.key = KEY;
  t.forge = true;                                                  // someone else answers
  await assert.rejects(bleWrite(t.exchange, keyFor, DEV, OP.interval, 600), /doğrulanamadı/);
  t.forge = false;
  // Signed INVALID is a rejection (the op is closed as "rejected"), not a lost write.
  await assert.rejects(bleSettingWriter(t.exchange, keyFor, DEV).bleWindow(301), DeviceRejected);
  assert.equal((await bleWrite(t.exchange, keyFor, DEV, OP.bleWindow, 301)).outcome, "INVALID");
  assert.equal(t.config.ble, 30);
  // No answer: never assume it was written.
  const silent: Exchange = async (type) => { if (type === WRITE.challenge) return challengeMsg(); throw new Error("Cihaz yanıt vermedi."); };
  await assert.rejects(bleWrite(silent, keyFor, DEV, OP.interval, 600), /varsayma/);
});

test("delivery over Bluetooth closes the settings op on the tasma's own answer", async () => {
  const t = new FakeTracker();
  const results: string[] = [], reported: unknown[] = [];
  const deps = (wanted: Record<string, number | boolean>) => ({
    wanted, has: { interval_s: 180 }, openSettings: { op: "op1", payload: {} } as never,
    reportSettings: async (v: unknown) => { reported.push(v); },
    result: async (op: string, state: string, note?: string) => { results.push(`${op}:${state}${note ? ":" + note.slice(0, 12) : ""}`); },
  });
  let out = await deliverSettings(bleSettingWriter(t.exchange, keyFor, DEV), deps({ interval_s: 600 }));
  assert.deepEqual(out, { wrote: ["ayarlar"], problems: [] });
  assert.deepEqual(reported, [{ interval_s: 600 }]);
  out = await deliverSettings(bleSettingWriter(t.exchange, keyFor, DEV), deps({ interval_s: 30 }));
  assert.equal(out.wrote.length, 0);
  assert.match(out.problems[0], /1 dakika/);                      // refused before anything is sent
  out = await deliverSettings(bleSettingWriter(t.exchange, () => null, DEV), deps({ interval_s: 900 }));
  assert.match(out.problems[0], /yazma anahtarı yok/);
  assert.deepEqual(results, ["op1:applied"]);                      // the op stays open until the tasma answers
});

test("write keys: from the account, by epoch, gone at sign-out", async () => {
  const mem = new Map<string, string>();
  const store = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => { mem.set(k, v); }, removeItem: (k: string) => { mem.delete(k); } };
  const book = new WriteKeyBook(store);
  const server = { writeKeys: async () => [
    { device: DEV, epoch: 1, writeKey: hex(KEY) },
    { device: "bad", epoch: 1, writeKey: hex(KEY) },
    { device: "52E16773141A8887", epoch: 0, writeKey: hex(KEY) },
  ] };
  assert.equal(await book.refresh(server), 1);
  assert.equal(book.size, 1);
  assert.deepEqual(book.get(DEV.toLowerCase(), 1), KEY);
  assert.equal(book.get(DEV, 2), null);
  assert.ok(book.has(DEV) && !book.has("52E16773141A8887"));
  assert.deepEqual(new WriteKeyBook(store).get(DEV, 1), KEY);       // survives a restart (offline field use)
  assert.equal(await book.refresh(server), 0);
  // The server's list wins: a re-added tasma's old epoch goes away.
  assert.equal(await book.refresh({ writeKeys: async () => [{ device: DEV, epoch: 2, writeKey: "AB".repeat(32) }] }), 2);
  assert.equal(book.get(DEV, 1), null);
  book.clear();
  assert.equal(book.size, 0);
  assert.equal(new WriteKeyBook(store).size, 0);
});

// ---- 4c-2: alan listesi --------------------------------------------------
const SQUARE: E7[] = [[378500000, 281200000], [378500000, 281210000], [378510000, 281210000], [378510000, 281200000]];
const TRIANGLE: E7[] = [[378520000, 281200000], [378520000, 281210000], [378530000, 281205000]];
const zone = (id: string, ring: E7[]): Zone => ({ id, name: id, ring: ring.map(([la, lo]) => [lo / 1e7, la / 1e7]), active: true, updated: 0 });

test("4c-2 shared vector: staged-list digest and the commit tag", () => {
  assert.equal(hex(stagedDigest([SQUARE, TRIANGLE])), "E3D8EEB9E11497E1F05A30003A76EE6DFAC83EC7E89EE742F013358845C06D29");
  const req = writeRequest(KEY, parseChallenge(challengeMsg()), OP.fence, 2 | (7 << 8), stagedDigest([SQUARE, TRIANGLE]));
  assert.equal(hex(req), "0402070000" + "704796AD04E2404AAF1B51D30C3CCD54");
});

test("the area list goes over Bluetooth: staged in pieces, one signed commit", async () => {
  const t = new FakeTracker();
  const many: E7[] = Array.from({ length: 12 }, (_, i) => [378500000 + 1000 * Math.round(Math.cos(i / 2) * 50), 281200000 + 1000 * Math.round(Math.sin(i / 2) * 50)]);
  const sum = await bleWriteFence(t.exchange, keyFor, DEV, [zone("a", SQUARE), zone("b", TRIANGLE), zone("c", many)]);
  assert.deepEqual([sum.areas, sum.vertices], [3, 19]);
  assert.deepEqual(t.fence, { state: 2, areas: 3, vertices: 19, revision: 2 });
  assert.equal(t.stageSteps, 8);   // begin, square, area, triangle, area, 12 points in 3 messages
  assert.equal(t.staged, null);
  // The tasma refuses the list (signed INVALID): a rejection; its list stays.
  await assert.rejects(bleWriteFence(t.exchange, keyFor, DEV, [zone("x", [[378500000, 281200000], [378500000, 281210000]])]), DeviceRejected);
  assert.deepEqual([t.fence.areas, t.fence.vertices], [3, 19]);
  // No key: nothing committed, staging aborted.
  await assert.rejects(bleWriteFence(t.exchange, () => null, DEV, [zone("a", SQUARE)]), NoWriteKey);
  assert.equal(t.staged, null);
  assert.deepEqual([t.fence.areas, t.fence.vertices], [3, 19]);
  // Someone else answers the commit: never "written".
  t.forge = true;
  await assert.rejects(bleWriteFence(t.exchange, keyFor, DEV, [zone("a", SQUARE)]), /doğrulanamadı/);
  t.forge = false;
  // Too many corners never leaves the phone.
  const big: E7[] = Array.from({ length: 65 }, (_, i) => [378500000 + i, 281200000 + (i % 2)]);
  await assert.rejects(bleWriteFence(t.exchange, keyFor, DEV, [zone("big", big)]), /64 köşe/);
});

test("Bluetooth delivery: settings first, then the open area op, both closed on the tasma's answer", async () => {
  const t = new FakeTracker();
  const results: string[] = [], recorded: unknown[] = [];
  const out = await deliverBle(t.exchange, keyFor, DEV, {
    wanted: { interval_s: 600 }, has: { interval_s: 180 },
    openSettings: { op: "s1", payload: {} } as never,
    openFence: { op: "f1", payload: { zones: [{ id: "a", name: "Mera", ring: zone("a", SQUARE).ring }] } } as never,
    reportSettings: async () => undefined,
    recordFence: async (sum, zones) => { recorded.push([sum.areas, sum.vertices, zones]); },
    result: async (op, state) => { results.push(`${op}:${state}`); },
  });
  assert.deepEqual(out, { wrote: ["ayarlar", "alanlar (1)"], problems: [] });
  assert.deepEqual(results, ["s1:applied", "f1:applied"]);
  assert.deepEqual(recorded, [[1, 4, [{ id: "a", name: "Mera" }]]]);
  assert.deepEqual([t.config.interval, t.fence.areas], [600, 1]);
});
