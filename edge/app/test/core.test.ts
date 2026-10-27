// ORUN Edge app core, host only: node --test --experimental-strip-types.
import assert from "node:assert/strict";
import { test } from "node:test";
import { concat, fromHex, hex } from "../src/core/bytes.ts";
import { aesCcmDecrypt, aesCcmEncrypt } from "../src/core/crypto.ts";
import { EdgeCore } from "../src/core/edge.ts";
import {
  acceptLine, acceptMac, acceptMessage, frames, Reassembler, REASSEMBLY_TIMEOUT_MS,
} from "../src/core/edgeLink.ts";
import { Ledger } from "../src/core/ledger.ts";
import { benchCredentials, openProductFrame, Reject } from "../src/core/product.ts";
import { SCHEMA } from "../src/core/db.ts";
import { KAT_EVENT, KAT_PERIODIC, seal, track } from "./fixtures.ts";
import { NodeDb } from "./nodeDb.ts";

const GW = 0x09a462bd4b275ba5n;

test("AES-CCM: RFC 3610 packet vector #1, tamper rejected", () => {
  const k = new Uint8Array(16).map((_, i) => 0xc0 + i);
  const n = fromHex("00000003020100A0A1A2A3A4A5");
  const a = new Uint8Array(8).map((_, i) => i);
  const p = new Uint8Array(23).map((_, i) => 8 + i);
  const c = aesCcmEncrypt(k, n, p, a, 8);
  assert.equal(hex(c), "588C979A61C663D2F066D0C2C0F989806D5F6B61DAC38417E8D12CFDF926E0");
  assert.deepEqual(aesCcmDecrypt(k, n, c, a, 8), p);
  for (let i = 0; i < c.length; i++) {
    const bad = c.slice(); bad[i] ^= 0x01;
    assert.equal(aesCcmDecrypt(k, n, bad, a, 8), null);
  }
  const badAad = a.slice(); badAad[0] ^= 1;
  assert.equal(aesCcmDecrypt(k, n, c, badAad, 8), null);
});

test("framing: shared vector with firmware and the PC tool", () => {
  const f = frames(new Uint8Array(40).map((_, i) => i), 9).map(hex);
  assert.deepEqual(f, [
    "01010900000102030405060708090A0B0C0D0E0F",
    "01000901101112131415161718191A1B1C1D1E1F",
    "010209022021222324252627",
  ]);
});

test("framing: every size round-trips; broken sequences are dropped", () => {
  const r = new Reassembler();
  for (let size = 1; size <= 160; size++) {
    const msg = new Uint8Array(size).map((_, i) => (i * 7 + 3) & 0xff);
    let out: Uint8Array | null = null;
    for (const fr of frames(msg, size)) { assert.ok(fr.length <= 20); out = r.push(fr); }
    assert.deepEqual(out, msg);
  }
  assert.throws(() => frames(new Uint8Array(161), 1));
  const [a, b, c] = frames(new Uint8Array(40).fill(5), 3);
  assert.equal(r.push(a), null);
  assert.equal(r.push(c), null);                       // gap
  assert.equal(r.push(b), null);                       // no longer active
  r.push(a);
  const foreign = b.slice(); foreign[2] = 4;
  assert.equal(r.push(foreign), null);
  r.push(a);
  const badVersion = b.slice(); badVersion[0] = 2;
  assert.equal(r.push(badVersion), null);
  assert.equal(r.push(c), null);
  // Oversize: 11 frames of 16 bytes with no END.
  const big = new Uint8Array(20).fill(1); big[1] = 1; big[2] = 7; big[3] = 0;
  r.push(big);
  for (let i = 1; i <= 10; i++) {
    const m = new Uint8Array(20).fill(1); m[1] = 0; m[2] = 7; m[3] = i;
    assert.equal(r.push(m), null);
  }
  const end = new Uint8Array(20).fill(1); end[1] = 2; end[2] = 7; end[3] = 11;
  assert.equal(r.push(end), null);
  // Expiry.
  r.push(a, 100);
  r.poll(100 + REASSEMBLY_TIMEOUT_MS);
  assert.equal(r.push(b, 100 + REASSEMBLY_TIMEOUT_MS), null);
});

test("edge accept MAC: shared vector with the firmware (SF5L5)", () => {
  const object = fromHex(
    "02040201030140000E8ADE7E71531AA30102030400000000000000010104070A0D101316191C1F2225282B2E3134373A3D404346494C4F5255585B5E6164676A6D707376797C7F8285888B8E9194979A9DA0A3A6A9ACAFB2B5B8BBBE5051525354555657");
  assert.equal(hex(acceptMac(GW, 1, 2, 3n, object)),
               "EE0E6E26790F83C816F6738CF71DF64B900A659F00E65C9DAD8C92B2E486C521");
  const msg = acceptMessage(GW, 1, 2, 3n, object);
  assert.equal(msg.length, 45);
  assert.equal(hex(msg.subarray(0, 13)), "8200010002" + "0000000000000003");
  assert.equal(acceptLine(GW, 1, 2, 3n, object),
    "EDGE ACCEPT v1 page=1 slot=2 gen=0000000000000003 mac=EE0E6E26790F83C816F6738CF71DF64B900A659F00E65C9DAD8C92B2E486C521");
});

test("product frames: KAT open; tamper, unknown epoch and bad shape rejected", () => {
  const p = openProductFrame(KAT_PERIODIC(), benchCredentials);
  assert.equal(hex(p.deviceId), "1122334455667788");
  assert.equal(p.family, 1);
  assert.equal(p.sequence, 257);
  const e = openProductFrame(KAT_EVENT(), benchCredentials);
  assert.equal(e.family, 2);
  assert.equal(e.sequence, 258);
  const raw = KAT_PERIODIC();
  for (const i of [0, 9, 17, 27, 28, 60, 99]) {
    const bad = raw.slice(); bad[i] ^= 0x10;
    assert.throws(() => openProductFrame(bad, benchCredentials), Reject);
  }
  assert.throws(() => openProductFrame(raw, () => null), Reject);
  assert.throws(() => openProductFrame(raw.subarray(0, 99), benchCredentials), Reject);
});

async function freshDb(): Promise<NodeDb> {
  const db = new NodeDb();
  for (const s of SCHEMA) await db.execute(s);
  return db;
}

test("ledger: stored, duplicate, rejected, conflict quarantine, uncertain then retry", async () => {
  const db = await freshDb();
  const ledger = new Ledger(db, benchCredentials);
  const [f0, f1] = track(2);
  assert.equal((await ledger.ingest(f0)).state, "STORED");
  assert.equal((await ledger.ingest(f0)).state, "DUPLICATE");
  const bad = f0.slice(); bad[50] ^= 1;
  assert.equal((await ledger.ingest(bad)).state, "REJECTED");
  // Same identity (device, incarnation, sequence), different authenticated content.
  const plain = openProductFrame(f0, benchCredentials).plaintext.slice();
  plain[57] ^= 1;                                         // battery mV
  const other = seal(KAT_PERIODIC(), plain, 900);
  assert.equal((await ledger.ingest(other)).state, "CONFLICT");
  assert.equal((await ledger.ingest(other)).state, "CONFLICT");
  assert.equal((await ledger.ingest(f0)).state, "CONFLICT");   // the winner is quarantined too
  assert.equal(Number((await db.query("SELECT count(*) AS n FROM conflicts"))[0].n), 1);
  db.failNextCommit = true;
  assert.equal((await ledger.ingest(f1)).state, "UNCERTAIN");
  assert.equal(await ledger.recordCount(), 1);
  assert.equal((await ledger.ingest(f1)).state, "STORED");
  // A damaged stored frame never yields a duplicate success.
  await db.run("UPDATE observations SET frame=? WHERE sequence=1001",
               [hex(concat(f1.subarray(0, 99), new Uint8Array([f1[99] ^ 1])))]);
  assert.equal((await ledger.ingest(f1)).state, "UNCERTAIN");
});

function heldLine(gw: bigint, page: number, slot: number, gen: bigint, obj: Uint8Array) {
  return `EDGE HELD v1 gw=${gw.toString(16).toUpperCase().padStart(16, "0")} page=${page} slot=${slot} ` +
    `gen=${gen.toString(16).toUpperCase().padStart(16, "0")} len=${obj.length} obj=${hex(obj)}`;
}

test("USB: RX signal pairing, accept after commit, BUSY/timeout retry, OK/REJECTED", async () => {
  let now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [f0, f1, f2] = track(3);
  assert.equal(await edge.handleLine("RX PRODUCT_SECURE queued family=1 length=100 rssi=-91 snr=6"), null);
  await edge.handleLine("BENCH GW RX record; durable custody write started");
  const out = await edge.handleLine(heldLine(GW, 0, 5, 7n, f0));
  assert.match(out!, /^YENİ  tasma=1122334455667788 seq=1000 .* \| sinyal -91 dBm SNR 6$/);
  // A DUMP line does not inherit a stranger's signal.
  await edge.handleLine("RX PRODUCT_SECURE queued family=1 length=100 rssi=-50 snr=9");
  await edge.handleLine("BENCH GW RX duplicate already held");
  assert.doesNotMatch((await edge.handleLine(heldLine(GW, 0, 6, 7n, f1)))!, /sinyal/);

  const line = edge.nextAcceptLine(now)!;
  assert.equal(line, acceptLine(GW, 0, 5, 7n, f0));
  assert.equal(edge.nextAcceptLine(now + 1), null);              // one in flight
  assert.equal(edge.nextAcceptLine(now + 3), line);              // silence: again
  await edge.handleLine("EDGE ACCEPT BUSY page=? slot=?");
  assert.equal(edge.nextAcceptLine(now + 3), line);
  await edge.handleLine("EDGE ACCEPT OK page=9 slot=9");          // not ours: ignored
  assert.equal(edge.pendingAccepts(), 2);
  await edge.handleLine("EDGE ACCEPT OK page=0 slot=5");
  assert.equal(edge.handedOff, 1);
  assert.equal(edge.nextAcceptLine(now + 3), acceptLine(GW, 0, 6, 7n, f1));
  // Exactly what the Gateway prints (sf5l0_bench_gateway.cpp edgeAccept).
  const warn = await edge.handleLine("EDGE ACCEPT REJECTED reason=MAC page=0 slot=6");
  assert.match(warn!, /^UYARI gateway teslimi reddetti page=0 slot=6/);
  assert.equal(edge.nextAcceptLine(now + 10), null);             // not retried
  assert.equal(edge.acceptRejected, 1);
  assert.match((await edge.handleLine("EDGE RECLAIM page=0 OK"))!, /sayfa 0 boşaltıldı/);
  // A truncated line is rejected, never guessed.
  const broken = heldLine(GW, 1, 0, 8n, f2).slice(0, -2);
  assert.match((await edge.handleLine(broken))!, /^RED/);
  assert.equal(edge.counts.REJECTED, 1);
  assert.equal(edge.pendingAccepts(), 0);
  // Commit failure: no accept for that record.
  const db = new NodeDb();
  const e2 = new EdgeCore(db, { credentials: benchCredentials, clock: () => now });
  await e2.init();
  db.failNextCommit = true;
  assert.match((await e2.handleLine(heldLine(GW, 1, 0, 8n, f2)))!, /^BELİRSİZ/);
  assert.equal(e2.pendingAccepts(), 0);
  now += 1;
});

function heldMsg(page: number, slot: number, gen: bigint, obj: Uint8Array,
                 link: [number, number] | null): Uint8Array {
  const h = new Uint8Array(18);
  const v = new DataView(h.buffer);
  h[0] = 2; v.setUint16(1, page); v.setUint16(3, slot); v.setBigUint64(5, gen);
  if (link) { h[13] = 1; v.setInt16(14, link[0]); v.setInt8(16, link[1]); }
  h[17] = obj.length;
  return concat(h, obj);
}

test("BLE: HELLO first, HELD with link, binary ACCEPT, results, reclaim, dump end", async () => {
  const now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [f0] = track(1);
  assert.equal(await edge.handleMessage(heldMsg(0, 1, 2n, f0, null)), null);  // before HELLO
  const hello = concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00010420"));
  assert.match((await edge.handleMessage(hello))!, /09A462BD4B275BA5 bağlı; tuttuğu kayıt 1\/1056/);
  const out = await edge.handleMessage(heldMsg(0, 1, 2n, f0, [-97, -3]));
  assert.match(out!, /^YENİ .* \| sinyal -97 dBm SNR -3$/);
  const accept = edge.nextAcceptMessage(now)!;
  assert.deepEqual(accept, acceptMessage(GW, 0, 1, 2n, f0));
  assert.equal(await edge.handleMessage(fromHex("040000000103")), null);       // BUSY
  assert.deepEqual(edge.nextAcceptMessage(now), accept);
  await edge.handleMessage(fromHex("040000000100"));                          // handed off
  assert.equal(edge.handedOff, 1);
  assert.match((await edge.handleMessage(fromHex("05000201")))!, /sayfa 2 boşaltıldı/);
  assert.match((await edge.handleMessage(fromHex("05000200")))!, /^UYARI gateway sayfa 2 silinemedi/);
  assert.match((await edge.handleMessage(fromHex("030001")))!, /DUMP END count=1 \| ÖZET yeni=1 .* teslim=1 teslim_red=0 bekleyen_teslim=0/);
  // Session end: the next connection starts from HELLO again.
  await edge.sessionEnd();
  assert.equal(await edge.handleMessage(heldMsg(0, 1, 2n, f0, null)), null);
});

test("station held count stays live: HELLO, then announced minus handed off", async () => {
  const now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [f0, f1, f2] = track(3);
  assert.equal(edge.heldNow(), null);                                          // nothing told yet
  await edge.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00020420")));
  assert.equal(edge.heldNow(), 2);                                             // HELLO: 2 held
  await edge.handleMessage(heldMsg(0, 1, 2n, f0, null));
  edge.nextAcceptMessage(now);
  await edge.handleMessage(fromHex("040000000100"));                           // handed off
  assert.equal(edge.heldNow(), 1);                                             // before the pass ends: HELLO - OK
  await edge.handleMessage(heldMsg(0, 2, 2n, f1, null));
  await edge.handleMessage(fromHex("030002"));                                 // DUMP END
  assert.equal(edge.heldNow(), 1);                                             // exact: announced - handed off
  await edge.handleMessage(heldMsg(1, 0, 3n, f2, [-90, 5]));                   // a new live record
  assert.equal(edge.heldNow(), 2);
  edge.nextAcceptMessage(now);
  await edge.handleMessage(fromHex("040000000200"));                           // page 0 slot 2 handed off
  assert.equal(edge.heldNow(), 1);
  await edge.handleMessage(fromHex("05000101"));                               // page 1 reclaimed
  assert.equal(edge.heldNow(), 0);
  await edge.sessionEnd();
  assert.equal(edge.heldNow(), 0);                                             // last HELLO kept, nothing announced
});

test("BLE: incomplete DUMP END cannot silently undercount held records", async () => {
  const now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [f0, f1] = track(2);
  await edge.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00020420")));
  assert.equal(edge.heldNow(), 2);

  // Two records still on the Gateway, but the phone misses one HELD message.
  await edge.handleMessage(heldMsg(0, 1, 2n, f0, null));
  await edge.handleMessage(heldMsg(0, 1, 2n, f0, null)); // duplicate is NOT the second record
  const warning = await edge.handleMessage(fromHex("030002"));
  assert.match(warning!, /UYARI: eksik aktarım/);
  assert.equal(edge.heldNow(), 2); // never claim "1 left" from an incomplete scan

  // A subsequent complete pass restores an exact count.
  await edge.handleMessage(heldMsg(0, 1, 2n, f0, null));
  await edge.handleMessage(heldMsg(0, 2, 2n, f1, null));
  assert.doesNotMatch((await edge.handleMessage(fromHex("030002")))!, /UYARI: eksik aktarım/);
  assert.equal(edge.heldNow(), 2);
  edge.nextAcceptMessage(now);
  await edge.handleMessage(fromHex("040000000100"));
  assert.equal(edge.heldNow(), 1);
});

test("USB: incomplete DUMP END count is not treated as exact zero", async () => {
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials });
  await edge.init();
  const [f0, f1] = track(2);
  assert.equal(edge.heldNow(), null);
  await edge.handleLine(heldLine(GW, 0, 1, 2n, f0));
  const warning = await edge.handleLine("EDGE DUMP END count=2");
  assert.match(warning!, /UYARI: eksik\/bozuk aktarım/);
  assert.equal(edge.heldNow(), 2); // reported count remains a conservative floor

  await edge.handleLine(heldLine(GW, 0, 1, 2n, f0));
  await edge.handleLine(heldLine(GW, 0, 2, 2n, f1));
  assert.doesNotMatch((await edge.handleLine("EDGE DUMP END count=2"))!, /UYARI:/);
  assert.equal(edge.heldNow(), 2);
});

test("redump: previously delivered live events do not spoil the next census", async () => {
  const now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [f0, f1] = track(2);
  await edge.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00010420")));
  await edge.handleMessage(heldMsg(0, 1, 2n, f0, null));
  await edge.handleMessage(fromHex("030001")); // complete first pass
  edge.nextAcceptMessage(now);
  await edge.handleMessage(fromHex("040000000100"));
  assert.equal(edge.heldNow(), 0);

  // A fresh record arrives and is handed off before a user-initiated redump.
  await edge.handleMessage(heldMsg(1, 0, 3n, f1, null));
  edge.nextAcceptMessage(now);
  await edge.handleMessage(fromHex("040001000000"));
  assert.equal(edge.heldNow(), 0);

  await edge.beginDump();
  assert.doesNotMatch((await edge.handleMessage(fromHex("030000")))!, /UYARI/);
  assert.equal(edge.heldNow(), 0);
});

test("BLE latest-head preview arrives first, saves immediately, ACKs securely and does not inflate the dump census", async () => {
  const now = 1_800_000_000;
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const [older, newest] = track(2);
  const hello = concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00020420"));
  await edge.handleMessage(hello);
  // A new firmware sends type 0x06 before the chronological 0x02 backlog.
  const early = heldMsg(0, 2, 2n, newest, null);
  early[0] = 0x06;
  assert.match((await edge.handleMessage(early))!, /^YENİ/);
  assert.equal(await edge.ledger.recordCount(), 1);
  assert.deepEqual(edge.nextAcceptMessage(now), acceptMessage(GW, 0, 2, 2n, newest));
  await edge.handleMessage(fromHex("040000000200")); // ACK latest
  assert.equal(edge.heldNow(), 1);
  assert.match((await edge.handleMessage(heldMsg(0, 1, 2n, older, null)))!, /^YENİ/);
  assert.match((await edge.handleMessage(heldMsg(0, 2, 2n, newest, null)))!, /^VARDI/);
  const end = await edge.handleMessage(fromHex("030002")); // exact chronological census
  assert.match(end!, /DUMP END count=2/);
  assert.doesNotMatch(end!, /UYARI: eksik aktarım/);
  assert.equal(await edge.ledger.recordCount(), 2);
});

test("USB priority preview preserves chronological DUMP_END and never borrows an RX RSSI", async () => {
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials });
  await edge.init();
  const [older, newest] = track(2);
  const priority = heldLine(GW, 0, 2, 2n, newest).replace("EDGE HELD", "EDGE PRIORITY");
  await edge.handleLine("RX PRODUCT_SECURE queued family=1 length=100 rssi=-44 snr=11");
  assert.doesNotMatch((await edge.handleLine(priority))!, /sinyal/);
  assert.equal(await edge.ledger.recordCount(), 1);
  assert.match((await edge.handleLine(heldLine(GW, 0, 1, 2n, older)))!, /^YENİ/);
  assert.match((await edge.handleLine(heldLine(GW, 0, 2, 2n, newest)))!, /^VARDI/);
  assert.doesNotMatch((await edge.handleLine("EDGE DUMP END count=2"))!, /UYARI:/);
  assert.equal(await edge.ledger.recordCount(), 2);
});

test("unknown or corrupted preview is ignored and cannot corrupt ACCEPT or history accounting", async () => {
  const edge = new EdgeCore(await freshDb(), { credentials: benchCredentials });
  await edge.init();
  const [object] = track(1);
  await edge.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00010420")));
  const malformed = heldMsg(0, 0, 1n, object, null);
  malformed[0] = 0x06;
  malformed[17] = 110;
  assert.equal(await edge.handleMessage(malformed), null);
  assert.equal(await edge.ledger.recordCount(), 0);
  assert.equal(edge.pendingAccepts(), 0);
  assert.equal(await edge.handleMessage(new Uint8Array([0x7F])), null);
  assert.match((await edge.handleMessage(heldMsg(0, 0, 1n, object, null)))!, /^YENİ/);
  assert.doesNotMatch((await edge.handleMessage(fromHex("030001")))!, /UYARI:/);
});

test("points: live / through / cut / unknown classification", async () => {
  let now = 0;
  const db = await freshDb();
  const edge = new EdgeCore(db, { credentials: benchCredentials, clock: () => now });
  await edge.init();
  const fs = track(5);
  const t0 = openProductFrame(fs[0], benchCredentials);
  const rec = (i: number) => new DataView(openProductFrame(fs[i], benchCredentials).plaintext.buffer).getUint32(16);
  const hello = concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00000420"));
  // Session 1 covers capture times rec0+5 .. rec3+10 (records are 60 s apart).
  now = rec(0) + 5; await edge.sessionAlive("ble"); await edge.handleMessage(hello);
  await edge.handleMessage(heldMsg(0, 0, 1n, fs[0], [-80, 7]));     // live
  now = rec(1) + 100; await edge.handleMessage(heldMsg(0, 1, 1n, fs[1], null));  // through
  now = rec(3) + 10; await edge.sessionAlive("ble"); await edge.sessionEnd();
  // Later, over USB: 2 is late but a live record is 120 s away (queue wait);
  // 3 is late with no live record within 150 s (cut); 4 was captured while
  // nobody listened (unknown).
  now = rec(4) + 400; await edge.sessionAlive("usb");
  await edge.handleLine(heldLine(GW, 0, 2, 1n, fs[2]));
  await edge.handleLine(heldLine(GW, 0, 3, 1n, fs[3]));
  await edge.handleLine(heldLine(GW, 0, 4, 1n, fs[4]));
  const r = await edge.points();
  assert.equal(r.count, 5);
  const by = new Map(r.points.map((p) => [p.seq, p]));
  assert.equal(by.get(1000)!.link, "live");
  assert.equal(by.get(1000)!.rssi, -80);
  assert.equal(by.get(1000)!.delay_s, 5);
  assert.equal(by.get(1001)!.link, "through");
  assert.equal(by.get(1002)!.link, null);
  assert.equal(by.get(1003)!.link, "cut");
  assert.equal(by.get(1004)!.link, null);
  assert.equal(by.get(1004)!.delay_s, 400);
  assert.equal(by.get(1000)!.tracker, hex(t0.deviceId));
});
