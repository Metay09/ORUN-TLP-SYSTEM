import assert from "node:assert/strict";
import { test } from "node:test";
import { identityFault, validateIdentity } from "../src/platform/orunIdentity.ts";

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
  ]) assert.throws(() => validateIdentity(bad as never), Error);
});

test("Native identity module exposes no user or Tracker authority just from a public key",()=>{
  const valid = validateIdentity({present:true,installationId:"edge-"+"c".repeat(64),
    fingerprint:"B".repeat(20),publicKeyBase64:"C".repeat(43)+"="});
  assert.equal(Object.hasOwn(valid,"token"),false);
  assert.equal(Object.hasOwn(valid,"trackerRootKey"),false);
  assert.equal(Object.hasOwn(valid,"applied"),false);
});

test("Stage errors are safe, distinguish read from create and redact raw values", () => {
  const cases = [
    ["ORUN_IDENTITY_READ_FAILED", "READ_FAILED"],
    ["ORUN_IDENTITY_CREATE_FAILED", "CREATE_FAILED"],
    ["ORUN_IDENTITY_PUBLIC_KEY_FAILED", "PUBLIC_KEY_FAILED"],
    ["ORUN_IDENTITY_SIGN_VERIFY_FAILED", "SIGN_VERIFY_FAILED"],
    ["ORUN_IDENTITY_IDENTITY_FORMAT_FAILED", "IDENTITY_FORMAT_FAILED"],
  ] as const;
  for (const [native, mapped] of cases) {
    assert.equal(identityFault({code:native, message:"secret-not-to-display"}), mapped);
    assert.equal(identityFault({message:native}), mapped);
  }
  assert.equal(identityFault({message:"OrunIdentity plugin is not implemented on android"}),
    "BRIDGE_UNAVAILABLE");
  assert.equal(identityFault(new Error("Telefon kimliği doğrulanamadı.")),
    "RESPONSE_INVALID");
  assert.equal(identityFault({message:"Bearer example-sensitive-credential"}),
    "UNKNOWN");
  assert.equal(identityFault({}),"UNKNOWN");
});
