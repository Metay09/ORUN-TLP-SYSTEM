import assert from "node:assert/strict";
import { test } from "node:test";
import { crc16, type DfuSerial } from "../src/app/firmwareUsb.ts";
import { previousFirmwareUpdate, reconcileFirmwareUpdate, updateFirmware, type FirmwareUpdatePorts } from "../src/app/firmwareUpdate.ts";
import type { FirmwareEvidence } from "../src/app/firmwareSafety.ts";
import type { FirmwarePackage } from "../src/app/firmwarePackage.ts";

const id = "52E16773141A8887";
const sha = "0123456789abcdef0123456789abcdef01234567";
const hash = "f".repeat(64);
function evidence(fw = "0.5-old", epoch = "00000001", nextSeq = 10): FirmwareEvidence {
  return { id, kind: "tracker", firmware: fw, credential: "A".repeat(32), epoch,
    granted: "09A462BD4B275BA5", grantGeneration: "00000005", storeInc: "ABC",
    nextSeq, storeReady: true, storeFaulted: false };
}
const pkg = { target: "RAK4630", sourceCommit: sha, imageSha256: hash,
  image: Uint8Array.of(1,2,3), initPacket: Uint8Array.of(4) } as FirmwarePackage;
function storage() {
  const map = new Map<string,string>();
  return { map, getItem: (k: string) => map.get(k) ?? null,
    setItem: (k: string, v: string) => { map.set(k,v); } };
}
function ack(seq: number): Uint8Array {
  const h0 = ((seq + 1) & 7) << 3;
  return Uint8Array.from([0xc0, h0, 0, 0, (-h0) & 255, 0xc0]);
}
function ports(after: FirmwareEvidence, bad = false) {
  let reads = 0, entries = 0, sent = 0, closed = 0;
  const link: DfuSerial & {close(): Promise<void>} = {
    async write() { sent++; },
    async read() { return ack(bad ? 7 : sent & 7); },
    async close() { closed++; },
  };
  const data: FirmwareUpdatePorts = {
    async readApplication() { reads++; return reads === 1 ? evidence() : after; },
    async enterBootloader() { entries++; return link; },
  };
  return { data, get reads() { return reads; }, get entries() { return entries; },
    get sent() { return sent; }, get closed() { return closed; } };
}

test("journals BEFORE bootloader entry and confirms only after all readback checks", async () => {
  const st = storage(), p = ports(evidence("0.5-0123456", "00000001", 11));
  const x: FirmwareUpdatePorts = { ...p.data, enterBootloader: async (device) => {
    assert.equal(device, id);
    assert.equal(previousFirmwareUpdate(st, id)?.phase, "uncertain");
    return p.data.enterBootloader(device);
  }};
  const result = await updateFirmware(st, x, id, pkg);
  assert.equal(result.phase, "confirmed");
  assert.equal(p.reads, 2); assert.equal(p.entries, 1);
  assert.equal(p.sent, 4); assert.equal(p.closed, 1);
  assert.equal(result.bytesAcknowledged, 3);
});

test("malformed ACK remains uncertain; never automatically repeats firmware writes", async () => {
  const st = storage(), p = ports(evidence("0.5-0123456"), true);
  await assert.rejects(updateFirmware(st, p.data, id, pkg), /DFU aktarım sonucu belirsiz/);
  assert.equal(previousFirmwareUpdate(st, id)?.phase, "uncertain");
  assert.equal(p.sent, 1);
  await assert.rejects(updateFirmware(st, p.data, id, pkg), /Önceki firmware/);
  assert.equal(p.entries, 1);
});

test("changed key epoch blocks success even when firmware matches", async () => {
  const st = storage(), p = ports(evidence("0.5-0123456", "00000009", 11));
  await assert.rejects(updateFirmware(st, p.data, id, pkg), /Anahtar kimliği veya dönemi/);
  assert.equal(previousFirmwareUpdate(st, id)?.phase, "uncertain");
  assert.equal(p.closed, 1);
});

test("after restart read-only reconciliation can confirm but does not reflash", async () => {
  const st = storage(), first = ports(evidence("0.5-old"), true);
  await assert.rejects(updateFirmware(st, first.data, id, pkg));
  const readOnly = { readApplication: async () => evidence("0.5-0123456", "00000001", 20) };
  const result = await reconcileFirmwareUpdate(st, readOnly, id);
  assert.equal(result.phase,"confirmed");
  assert.equal(first.entries,1);
});

test("wrong USB device fails before journal or flash", async () => {
  const st = storage();
  let writes = 0;
  const p: FirmwareUpdatePorts = {
    readApplication: async () => ({...evidence(), id:"1111111111111111"}),
    enterBootloader: async () => { writes++; throw Error("should not enter"); },
  };
  await assert.rejects(updateFirmware(st,p,id,pkg), /kimliği/);
  assert.equal(writes,0);
  assert.equal(previousFirmwareUpdate(st,id),null);
});


test("corrupted persisted record prevents another flash attempt", async () => {
  const st = storage(), p = ports(evidence("0.5-0123456"));
  st.setItem("orun.firmwareUpdate.v1." + id, "{not-valid-json");
  await assert.rejects(updateFirmware(st, p.data, id, pkg), /kaydı bozuk/);
  assert.equal(p.entries, 0);
  assert.equal(p.reads, 0);
});
