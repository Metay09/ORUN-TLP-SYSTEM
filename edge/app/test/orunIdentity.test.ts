import assert from "node:assert/strict";
import { test } from "node:test";
import { validateIdentity } from "../src/platform/orunIdentity.ts";

test("Android key identity response is validated before any application use", () => {
  assert.deepEqual(validateIdentity({ present: false }), { present: false });
  const valid = { present: true, installationId: "edge-" + "a".repeat(64),
    fingerprint: "A".repeat(20), publicKeyBase64: "A".repeat(43) + "=" };
  assert.deepEqual(validateIdentity(valid), valid);
  for (const bad of [
    null, {}, {present:"true"}, {present:true},
    {...valid, installationId:"phone-a"},
    {...valid, publicKeyBase64:"invalid"},
    {...valid, fingerprint:"0".repeat(19)},
  ]) assert.throws(() => validateIdentity(bad as never), /kimlik|Kimlik|yanıt/);
});

test("Native identity module exposes no user or Tracker authority just from a public key",()=>{
  const valid = validateIdentity({present:true,installationId:"edge-"+"c".repeat(64),
    fingerprint:"B".repeat(20),publicKeyBase64:"C".repeat(43)+"="});
  assert.equal(Object.hasOwn(valid,"token"),false);
  assert.equal(Object.hasOwn(valid,"trackerRootKey"),false);
  assert.equal(Object.hasOwn(valid,"applied"),false);
});
