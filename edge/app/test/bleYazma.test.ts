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
  bleSettingWriter, bleWrite, NoWriteKey, OP, parseChallenge, parseResult, servicesValue, WRITE,
  WriteRefused, writeRequest, type Exchange,
} from "../src/app/bleWrite.ts";
import { deliverSettings } from "../src/app/deliver.ts";
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

  readonly exchange: Exchange = async (type, payload) => {
    if (type === WRITE.challenge) {
      this.challenge = Uint8Array.from({ length: 16 }, (_, i) => (this.next + i) & 0xff);
      this.next += 0x10;
      return challengeMsg(this.device, this.epoch, this.challenge);
    }
    assert.equal(type, WRITE.write);
    const ch = this.challenge; this.challenge = null;                    // single use
    if (this.refuse !== null) return { type: ERROR_TYPE, correlation: 2, payload: Uint8Array.of(this.refuse, type) };
    if (!ch || payload.length !== 21 || hex(sign(this.key, "ORUN-BLE-WRITE-REQ-v1", this.device, this.epoch, ch, payload.subarray(0, 5))) !== hex(payload.subarray(5)))
      return { type: ERROR_TYPE, correlation: 2, payload: Uint8Array.of(3, type) };
    const op = payload[0], v = payload[1] | (payload[2] << 8) | (payload[3] << 16) | (payload[4] << 24);
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
