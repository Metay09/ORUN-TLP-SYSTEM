// Host-only TS transport regression for the not-yet-wired Android enrollment.
// Run: node --experimental-strip-types tools/ci/test_phone_enrollment_client.ts
import assert from "node:assert/strict";
import {
  beginPhoneEnrollment, finishPhoneEnrollment,
  beginPhoneRecovery, finishPhoneRecovery,
  type PhonePublicIdentity,
} from "../../edge/app/src/app/phoneEnrollment.ts";

const identity: PhonePublicIdentity = {
  algorithm: "P256-SHA256",
  installationId: "edge-" + "a".repeat(64),
  fingerprint: "A".repeat(20),
  publicKeyBase64: "AQID",
};
const good = {
  installation_id: identity.installationId,
  fingerprint: identity.fingerprint,
  algorithm: "P256-SHA256",
  request_id: "b".repeat(32),
  challenge_base64: "Q".repeat(43) + "=",
  expires_at: 1799999999,
};
let calls = 0;
const fake = (async (url: string, init: RequestInit) => {
  calls++;
  assert.equal(url, "https://example.test:8443/api/v1/enroll/begin");
  assert.equal(init.method, "POST");
  assert.equal(init.credentials, "omit");
  assert.equal(init.redirect, "error");
  return new Response(JSON.stringify(good), {
    status: 200, headers: { "Content-Type": "application/json" },
  });
}) as typeof fetch;
const pending = await beginPhoneEnrollment("https://example.test:8443/", identity, fake);
assert.equal(pending.installationId, identity.installationId);
assert.equal(calls, 1);

await assert.rejects(
  () => beginPhoneEnrollment("http://example.test/", identity, fake),
  /HTTPS/,
);
assert.equal(calls, 1);

const wrong = (async () => new Response(JSON.stringify({
  ...good, installation_id: "edge-" + "c".repeat(64),
}), { status: 200, headers: { "Content-Type": "application/json" } })) as typeof fetch;
await assert.rejects(() => beginPhoneEnrollment(
  "https://example.test:8443/", identity, wrong), /uyuşmuyor/);

let signed = 0;
const sign = async (requestId: string, challenge: string) => {
  signed++;
  assert.equal(requestId, good.request_id);
  assert.equal(challenge, good.challenge_base64);
  return { installationId: identity.installationId,
           signatureBase64: "AQID" };
};
const finishFetch = (async (url: string, init: RequestInit) => {
  assert.equal(url, "https://example.test:8443/api/v1/enroll/finish");
  const body = JSON.parse(String(init.body));
  assert.equal(body.owner_code, "Z".repeat(43));
  assert.equal(body.algorithm, "P256-SHA256");
  return new Response(JSON.stringify({
    installation_id: identity.installationId,
    access_token: "s".repeat(64), expires_in_seconds: 2592000,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;
const result = await finishPhoneEnrollment(
  "https://example.test:8443/", identity, pending, "Z".repeat(43),
  sign, finishFetch);
assert.equal(result.accessToken, "s".repeat(64));
assert.equal(signed, 1);

await assert.rejects(
  () => finishPhoneEnrollment("https://example.test:8443/", identity,
    pending, "bad-code", sign, finishFetch),
  /geçersiz/,
);
assert.equal(signed, 1);

const denied = (async () => new Response(
  '{"error":"not_approved_or_invalid"}',
  { status: 403, headers: { "Content-Type": "application/json" } },
)) as typeof fetch;
await assert.rejects(
  () => finishPhoneEnrollment("https://example.test:8443/", identity,
    pending, "Z".repeat(43), sign, denied),
  /onaylanmadı/,
);
// The recovery request is a distinct, explicitly owner-approved protocol.
const recoverFetch = (async (url: string, init: RequestInit) => {
  assert.equal(url, "https://example.test:8443/api/v1/enroll/recover/begin");
  assert.equal(init.method, "POST");
  return new Response(JSON.stringify(good), {
    status: 200, headers: { "Content-Type": "application/json" },
  });
}) as typeof fetch;
const recovery = await beginPhoneRecovery(
  "https://example.test:8443/", identity, recoverFetch);
assert.equal(recovery.recovery, true);
await assert.rejects(() => finishPhoneEnrollment(
  "https://example.test:8443/", identity, recovery, "Z".repeat(43),
  sign, finishFetch), /geçersiz/);
const recoverFinishFetch = (async (url: string, init: RequestInit) => {
  assert.equal(url, "https://example.test:8443/api/v1/enroll/recover/finish");
  assert.equal(init.method, "POST");
  return new Response(JSON.stringify({
    installation_id: identity.installationId,
    access_token: "u".repeat(64), expires_in_seconds: 3600,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;
const regained = await finishPhoneRecovery(
  "https://example.test:8443/", identity, recovery, "Z".repeat(43),
  sign, recoverFinishFetch);
assert.equal(regained.accessToken, "u".repeat(64));

console.log("ORUN_PHONE_ENROLLMENT_TS_HOST_PASS");
