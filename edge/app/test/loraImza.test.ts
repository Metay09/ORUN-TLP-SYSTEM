// 6c/6d: telefon LoRa komutunu kendisi imzalar (internetsiz köprü) — firmware
// ve sunucuyla ortak vektörler; tasmanın imzalı cevabıyla "Tasmaya işlendi".
import assert from "node:assert/strict";
import { test } from "node:test";
import { concat, fromHex, hex } from "../src/core/bytes.ts";
import { hmacSha256, hkdfSha256 } from "../src/core/crypto.ts";
import type { OpTarget } from "../src/app/deviceOps.ts";
import { LocalLoraSource } from "../src/app/loraLocal.ts";
import { areaCommand, entriesFor, parseResult, settingsCommand } from "../src/app/loraSign.ts";

const DEV = "0E8ADE7E71531AA3";
// firmware/tests/m7/test_lora_command.cpp, backend/tests/test_lora_command.py
const VECTOR = "020700010E8ADE7E71531AA300000001010203040BADC0FFEE12345600000001020100000384030000003C" +
  "EDD9ABC7127362652C2CE8D98CDCD396";
const VECTOR_AREAS = "020700020E8ADE7E71531AA3000000010A0B0C0D0102030405060708000000010001010003010316694E0010B8172" +
  "0166AD4A010B8172016694E0010B99DC06673E256DA8890748DFEF81325A81C72";

function key(): Uint8Array {
  const info = concat(new TextEncoder().encode("ORUN-BLE-WRITE-KEY-v1"), fromHex(DEV), new Uint8Array([0, 0, 0, 1]));
  const salt = Uint8Array.from({ length: 16 }, (_, i) => 0x10 + i);
  const root = Uint8Array.from({ length: 32 }, (_, i) => 0x40 + i);
  return hkdfSha256(root, salt, info, 32);
}

test("ortak vektörler: ayar ve alan komutu", () => {
  const k = key();
  assert.equal(hex(settingsCommand(k, DEV, 1, 0x01020304, { incarnation: 0x0BADC0FFEE123456n, revision: 1 },
    [[1, 900], [3, 60]])).toUpperCase(), VECTOR);
  const areas = areaCommand(k, DEV, 1, 0x0A0B0C0D, { incarnation: 0x0102030405060708n, revision: 1 },
    [[[376000000, 280500000], [376100000, 280500000], [376000000, 280600000]]]);
  assert.deepEqual(areas.map((f) => hex(f).toUpperCase()), [VECTOR_AREAS]);
  const big = Array.from({ length: 8 }, (_, a) => Array.from({ length: 8 }, (_, i) => [376000000 + a * 100000 + i, 280500000 + i] as [number, number]));
  assert.deepEqual(areaCommand(k, DEV, 1, 1, { incarnation: 0n, revision: 0 }, big).map((f) => f.length), [255, 255, 170]);
  assert.equal(areaCommand(k, DEV, 1, 1, { incarnation: 0n, revision: 0 }, [])[0].length, 54);
  assert.deepEqual(entriesFor({ relay: true }, { tracking: false }), [[2, 1 | (2 << 8)]]);
});

/** A tasma that answers like the firmware (settings only here). */
function fakeTasma() {
  const k = key();
  let inc = 0x0BADC0FFEE123456n, rev = 4, interval = 180;
  return {
    get interval() { return interval; },
    get revision() { return rev; },
    /** A USB/Bluetooth write: the settings move to the next revision. */
    writeOverBle() { rev += 1; return rev; },
    answer(frameHex: string): string {
      const f = fromHex(frameHex);
      const v = new DataView(f.buffer, f.byteOffset);
      const cid = v.getUint32(16);
      const ok = v.getBigUint64(20) === inc && v.getUint32(28) === rev;
      let outcome = 7;
      if (ok) { interval = v.getUint32(34); rev += 1; outcome = 0; }
      const body = new Uint8Array(40);
      const d = new DataView(body.buffer);
      body.set([2, 8, outcome, 1]);
      body.set(fromHex(DEV), 4);
      d.setUint32(12, 1); d.setUint32(16, cid); d.setBigUint64(20, inc); d.setUint32(28, rev);
      d.setUint32(32, interval); d.setUint16(36, 30); body[38] = 1; body[39] = 1;
      const tag = hmacSha256(k, concat(new TextEncoder().encode("ORUN-LORA-RES-v1"), body)).slice(0, 16);
      return hex(concat(body, tag)).toUpperCase();
    },
  };
}

test("internetsiz telefon: yoklama, sonra uygulama; işlem tasmanın imzasıyla işlenir", async () => {
  const k = key();
  const ops: OpTarget[] = [{ op: "op-x1", device: DEV, kind: "settings", payload: { set: { interval_s: 300 } },
    state: "waiting", createdAt: 0, updatedAt: 0, via: null, note: null }];
  const results: string[] = [];
  const reported: Record<string, unknown>[] = [];
  const store = new Map<string, string>();
  let n = 1;
  const src = new LocalLoraSource({
    ops: () => ops, key: () => ({ epoch: 1, key: k }), reported: () => null,
    opResult: async (_op, _d, state) => { results.push(state); if (state !== "gateway") ops[0].state = state; },
    settingsApplied: async (_d, values) => { reported.push(values); },
    areasApplied: async () => {}, random32: () => n++,
  }, { getItem: (x) => store.get(x) ?? null, setItem: (x, v) => { store.set(x, v); } });
  const tasma = fakeTasma();
  let [cmd] = await src.loraCommands();
  await src.loraEvents([{ kind: "queued", device: DEV, command_id: cmd.command_id }]);
  await src.loraEvents([{ kind: "result", hex: tasma.answer(cmd.frames[0]) }]);   // STALE: state learnt
  [cmd] = await src.loraCommands();
  await src.loraEvents([{ kind: "result", hex: tasma.answer(cmd.frames[0]) }]);
  assert.deepEqual(results, ["gateway", "applied"]);
  assert.equal(tasma.interval, 300);
  assert.equal(reported[0].interval_s, 180, "yoklamanın STALE cevabı da tasmadaki ayarları söyler");
  assert.equal(reported[1].interval_s, 300);
  assert.deepEqual(await src.loraCommands(), []);   // settled
  // A forged answer changes nothing.
  ops[0].state = "waiting";
  [cmd] = await src.loraCommands();
  const bad = tasma.answer(cmd.frames[0]).slice(0, -2) + "00";
  await src.loraEvents([{ kind: "result", hex: bad }]);
  assert.equal(ops[0].state, "waiting");
  assert.ok(parseResult(k, fromHex(tasma.answer(cmd.frames[0]))));
});

test("USB/Bluetooth yazımından sonra sürüm bildirilir: sonraki LoRa komutu yoklamasız, tek seferde", async () => {
  const k = key();
  const ops: OpTarget[] = [{ op: "op-a", device: DEV, kind: "settings", payload: { set: { interval_s: 300 } },
    state: "waiting", createdAt: 0, updatedAt: 0, via: null, note: null }];
  const results: string[] = [];
  let n = 1;
  const src = new LocalLoraSource({
    ops: () => ops, key: () => ({ epoch: 1, key: k }), reported: () => null,
    opResult: async (op, _d, state) => { results.push(`${op}:${state}`); const t = ops.find((o) => o.op === op); if (t && state !== "gateway") t.state = state; },
    settingsApplied: async () => {}, areasApplied: async () => {}, random32: () => n++,
  }, null);
  const tasma = fakeTasma();
  let [cmd] = await src.loraCommands();
  await src.loraEvents([{ kind: "result", hex: tasma.answer(cmd.frames[0]) }]);   // yoklama: durum öğrenildi
  [cmd] = await src.loraCommands();
  await src.loraEvents([{ kind: "result", hex: tasma.answer(cmd.frames[0]) }]);
  assert.deepEqual(results, ["op-a:applied"]);
  // Telefon tasmaya Bluetooth ile yazdı; tasmanın imzalı cevabındaki sürüm bildirilir.
  const rev = tasma.writeOverBle();
  ops.push({ op: "op-b", device: DEV, kind: "settings", payload: { set: { interval_s: 600 } },
    state: "waiting", createdAt: 1, updatedAt: 1, via: null, note: null });
  src.noteRevision(DEV, 1, rev);
  src.noteRevision(DEV, 1, rev - 3);   // eskisi bir şey değiştirmez
  [cmd] = await src.loraCommands();
  await src.loraEvents([{ kind: "result", hex: tasma.answer(cmd.frames[0]) }]);
  assert.deepEqual(results, ["op-a:applied", "op-b:applied"], "STALE turu yok");
  assert.equal(tasma.interval, 600);
});
