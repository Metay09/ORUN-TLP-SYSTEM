// ORUN S5B: host-only completion/origin/identity boundary regression.
// Node built-in fetch mocks; no native plugin, live endpoints, or credentials.
import assert from "node:assert/strict";
import { completePhoneEnrollment, completePhoneRecovery } from
  "../../edge/app/src/app/completePhoneEnrollment.ts";
import type { PhonePublicIdentity, PhoneEnrollmentPending } from
  "../../edge/app/src/app/phoneEnrollment.ts";
import type { MobileSessionInput } from
  "../../edge/app/src/platform/orunMobileSession.ts";

const identity: PhonePublicIdentity = {
  algorithm: "P256-SHA256",
  installationId: "edge-" + "a".repeat(64),
  fingerprint: "B".repeat(20),
  publicKeyBase64: "AQID",
};
const pending: PhoneEnrollmentPending = {
  algorithm: "P256-SHA256",
  installationId: identity.installationId,
  fingerprint: identity.fingerprint,
  requestId: "c".repeat(32),
  challengeBase64: "Q".repeat(43) + "=",
  expiresAt: 1799999999,
};
const secret = "s".repeat(64);
let saved: MobileSessionInput | null = null;
let signCalls = 0;
const sign = async (requestId: string, challengeBase64: string) => {
  signCalls++;
  assert.equal(requestId, pending.requestId);
  assert.equal(challengeBase64, pending.challengeBase64);
  return { installationId: identity.installationId,
           signatureBase64: "AQID" };
};
const save = async (session: MobileSessionInput) => { saved = session; };
const replies = (wrong = false) => (async (url: string, init: RequestInit) => {
  if (url.endsWith("/api/v1/enroll/finish")) {
    assert.equal(init.method, "POST");
    assert.equal(init.credentials, "omit");
    const body = JSON.parse(String(init.body));
    assert.equal(body.owner_code, "Z".repeat(43));
    return new Response(JSON.stringify({
      installation_id: identity.installationId,
      access_token: secret, expires_in_seconds: 3600,
    }), { status: 200, headers: { "Content-Type": "application/json" } });
  }
  assert.equal(url, "https://example.test:8443/api/v1/sync/identity");
  assert.equal((init.headers as Record<string, string>).Authorization,
               "Bearer " + secret);
  return new Response(JSON.stringify({
    site: "orun-main",
    installation: wrong ? "edge-" + "d".repeat(64) : identity.installationId,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;

const done = await completePhoneEnrollment(
  "https://example.test:8443", identity, pending, "Z".repeat(43),
  sign, save, replies());
assert.deepEqual(Object.keys(done).sort(),
                 ["expiresAt", "installation", "site"]);
assert.equal(done.site, "orun-main");
assert.equal(done.installation, identity.installationId);
assert.equal(saved?.accessToken, secret);
assert.equal(saved?.origin, "https://example.test:8443");
assert.equal(signCalls, 1);
saved = null;

await assert.rejects(() => completePhoneEnrollment(
  "https://example.test:8443", identity, pending, "Z".repeat(43),
  sign, save, replies(true)), /uyuşmuyor/);
assert.equal(saved, null);

await assert.rejects(() => completePhoneEnrollment(
  "http://example.test:8443", identity, pending, "Z".repeat(43),
  sign, save, replies()), /HTTPS/);
assert.equal(saved, null);

const badHttp = (async (_url: string, _init: RequestInit) =>
  new Response('{"error":"unauthorized"}', {
    status: 401, headers: { "Content-Type": "application/json" },
  })) as typeof fetch;
await assert.rejects(() => completePhoneEnrollment(
  "https://example.test:8443", identity, pending, "Z".repeat(43),
  sign, save, badHttp), /kabul etmedi/);
assert.equal(saved, null);

// Recovery uses a distinct endpoint, replaces the prior bearer only after
// owner approval, and returns no bearer to the UI.
const recovery = { ...pending, recovery: true as const };
const recoveredFetch = (async (url: string, init: RequestInit) => {
  if (url.endsWith("/api/v1/enroll/recover/finish")) {
    assert.equal(init.method, "POST");
    return new Response(JSON.stringify({
      installation_id: identity.installationId,
      access_token: "r".repeat(64),
      expires_in_seconds: 3600,
    }), { status: 200, headers: { "Content-Type": "application/json" } });
  }
  assert.equal(url, "https://example.test:8443/api/v1/sync/identity");
  return new Response(JSON.stringify({
    site: "orun-main", installation: identity.installationId,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;
saved = null;
const regained = await completePhoneRecovery(
  "https://example.test:8443", identity, recovery, "Z".repeat(43),
  sign, save, recoveredFetch);
assert.equal(regained.site, "orun-main");
assert.equal(saved?.accessToken, "r".repeat(64));

console.log("ORUN_PHONE_SECURE_SESSION_FLOW_TS_HOST_PASS");
