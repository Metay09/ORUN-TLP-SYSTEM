import assert from "node:assert/strict";
import { test } from "node:test";
import { crc16, encodeHci, decodeHciAck, SlipFrames, dfuPlan, transferDfu, DfuError } from "../src/app/firmwareUsb.ts";

const end = 0xc0;
const makeAck = (seq: number): Uint8Array => {
  const h0 = ((seq + 1) & 7) << 3;
  return Uint8Array.from([end, h0, 0, 0, (-h0) & 255, end]);
};

test("HCI CRC16 matches fixed Adafruit reference and SLIP escapes", () => {
  assert.equal(crc16(Uint8Array.of(1,2,3)), 44461);
  const p = encodeHci(Uint8Array.of(0xc0, 0xdb, 0), 1);
  assert.equal(p[0], 0xc0); assert.equal(p[p.length-1], 0xc0);
  assert.match(Buffer.from(p).toString("hex"), /dbdcdbdd/);
  assert.equal(p[1], 0xd1);
});

test("correct ACK is validated by header, CRC and expected sequence", () => {
  const ack = makeAck(1);
  assert.deepEqual(decodeHciAck(ack), { sequence:0, nextSequence:2 });
  assert.deepEqual([...ack], [0xc0, 0x10, 0, 0, 0xf0, 0xc0]);
  const wrong = ack.slice(); wrong[2] ^= 4;
  assert.throws(() => decodeHciAck(wrong), /doğrulanamadı/);
  assert.throws(() => decodeHciAck(Uint8Array.of(0xc0,0xdb,0,0xc0)), /geçersiz|tamamlanmamış/);
});

test("HCI SLIP splitter keeps partial frames and splits coalesced input", () => {
  const s = new SlipFrames(), a = makeAck(1), b = makeAck(2);
  assert.equal(s.push(Uint8Array.from([0,1,...a.slice(0,3)])).length, 0);
  const out = s.push(Uint8Array.from([...a.slice(3),...b]));
  assert.equal(out.length, 2);
  assert.equal(decodeHciAck(out[0]).nextSequence, 2);
  assert.equal(decodeHciAck(out[1]).nextSequence, 3);
});

test("only application image, correct start/init/data/stop command order", () => {
  const p = [...dfuPlan(new Uint8Array(513).fill(3), Uint8Array.of(5,6))];
  assert.deepEqual(p.map((x)=>x.stage), ["init","init","transfer","transfer","activate"]);
  assert.deepEqual([...p[0].data.slice(0,8)], [3,0,0,0,4,0,0,0]);
  assert.deepEqual([...p[0].data.slice(-4)], [1,2,0,0]);
  assert.deepEqual([...p[1].data], [1,0,0,0,5,6,0,0]);
  assert.equal(p[2].data.length, 516);
  assert.equal(p[3].data.length, 5);
  assert.deepEqual([...p[4].data], [5,0,0,0]);
});

test("any incomplete/mismatched acknowledgement is result-unknown, no retry", async () => {
  const packets: Uint8Array[] = [];
  const fake = { async write(p: Uint8Array) { packets.push(p); }, async read() { return makeAck(7); } };
  await assert.rejects(transferDfu(fake, { image: Uint8Array.of(1), initPacket: Uint8Array.of(1) } as never),
    (e: unknown) => e instanceof DfuError && e.uncertain === true && /uyuşmadı/.test(e.message));
  assert.equal(packets.length,1);
});

test("complete mocked HCI ACK sequence sends app-only frames and never treats ACK as post-flash proof", async () => {
  const frames: Uint8Array[] = [];
  const progress: string[] = [];
  const fake = {
    async write(f: Uint8Array) { frames.push(f); },
    async read() { return makeAck(frames[frames.length - 1][1] & 7); },
  };
  await transferDfu(fake, { image: Uint8Array.of(1,2,3), initPacket: Uint8Array.of(5) } as never,
    (p) => { progress.push(p.stage); });
  assert.equal(frames.length, 4);
  assert.deepEqual(progress, ["init", "erase", "init", "transfer", "activate"]);
});

test("published Nordic HCI START application packet matches byte-for-byte", () => {
  const payload = Uint8Array.of(3,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0x0c,0x5d,0,0);
  assert.equal(Buffer.from(encodeHci(payload,1)).toString("hex"),
    "c0d14e01e0030000000400000000000000000000000c5d0000c6fcc0");
});
