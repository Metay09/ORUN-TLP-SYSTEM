import assert from "node:assert/strict";
import { test } from "node:test";
import { compareFirmwareEvidence, parseFirmwareEvidence } from "../src/app/firmwareSafety.ts";
const sha = "0123456abcdef0123456abcdef0123456abcdef0";
const t = [
  "BENCH DEVICE id=52E16773141A8887 role=TRACKER security_region=READY gnss=yes",
  "BENCH TRACKER id=52E16773141A8887 test_credential=no granted_gateway=09A462BD4B275BA5 product_store=READY",
  "BENCH CREDENTIAL credential=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA epoch=00000001 grant_generation=00000005 custody_floor=00000001",
  "PRODUCT STORE ready=yes faulted=no inc=ABC next_seq=10 retained=7",
  "APP DEVICE id=1 fw=0.5-0123456 rev=2",
];
test("tracker baseline retains nonsecret identity/epoch and store counters", () => {
  const before = parseFirmwareEvidence(t);
  assert.equal(before.kind,"tracker"); assert.equal(before.nextSeq,10);
  const newer = parseFirmwareEvidence(t.map(x=>x.replace("next_seq=10","next_seq=12")));
  assert.equal(compareFirmwareEvidence(before,newer,sha).ok,true);
});
test("fails closed if firmware unknown, key epoch differs or records regress", () => {
  const before = parseFirmwareEvidence(t);
  const damaged = parseFirmwareEvidence(t.map(x=>x.replace("next_seq=10","next_seq=9").replace("epoch=00000001","epoch=00000002")));
  assert.equal(compareFirmwareEvidence(before,damaged,sha).ok,false);
  assert.throws(()=>parseFirmwareEvidence(t.filter(x=>!x.startsWith("PRODUCT STORE"))),/hafızası/);
});
test("rejects identity contradictions and unassigned devices", () => {
  assert.throws(()=>parseFirmwareEvidence([...t,"BENCH GATEWAY id=1111111111111111 custody_region=READY"]),/çelişkili/);
  assert.throws(()=>parseFirmwareEvidence(["BENCH DEVICE id=1111111111111111 role=UNASSIGNED","APP DEVICE id=1 fw=1"]),/kimliği/);
});
test("gateway needs preserved held count and signed grants", () => {
  const lines = ["BENCH DEVICE id=09A462BD4B275BA5 role=GATEWAY security_region=READY gnss=no",
    "BENCH GATEWAY id=09A462BD4B275BA5 custody_region=READY custody_ready=yes held=7/200",
    "BENCH OGK1 status=READY generation=1111111111111111", "APP DEVICE id=2 fw=0.5-0123456"];
  const before = parseFirmwareEvidence(lines);
  assert.equal(compareFirmwareEvidence(before, parseFirmwareEvidence(lines), sha).ok,true);
  const less = parseFirmwareEvidence(lines.map(x=>x.replace("held=7/200","held=6/200")));
  assert.equal(compareFirmwareEvidence(before,less,sha).ok,false);
});
