// Password-first Android transport acceptance: all HTTP responses synthetic.
import assert from "node:assert/strict";
import { OwnerAccountClient } from
  "../../edge/app/src/app/ownerAccountClient.ts";

const endpoint = "https://example.test:8443";
const credential = "p".repeat(64);
const namespace = "account-" + "a".repeat(64);
const password = "Synthetic-Password-For-Test-Only-2026";
let logins = 0;
let logouts = 0;
const transport = (async (url: string, init: RequestInit) => {
  assert.equal(init.credentials, "omit");
  assert.equal(init.redirect, "error");
  if (url.endsWith("/api/v1/account/login")) {
    ++logins;
    const headers = init.headers as Record<string, string>;
    assert.equal(headers.Authorization, undefined,
      "user login must NOT require a phone bearer");
    assert.equal(headers["Content-Type"], "application/json");
    assert.deepEqual(JSON.parse(String(init.body)), {
      username: "owner", password,
    });
    return new Response(JSON.stringify({
      site: "orun-main", actor: "owner", username: "owner",
      installation: namespace, account_access_token: credential,
      expires_at: Math.floor(Date.now() / 1000) + 3600,
    }), { status: 200, headers: { "Content-Type": "application/json" } });
  }
  if (url.endsWith("/api/v1/account/logout")) {
    ++logouts;
    assert.equal((init.headers as Record<string, string>).Authorization,
                 "Bearer " + credential);
    return new Response(JSON.stringify({signed_out:true}), {
      status: 200, headers: { "Content-Type": "application/json" },
    });
  }
  assert.equal(url, endpoint + "/api/v1/sync?cursor=0");
  assert.equal((init.headers as Record<string, string>).Authorization,
               "Bearer " + credential);
  return new Response(JSON.stringify({
    cursor: 0, changes: [], has_more: false,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;

const a = new OwnerAccountClient(endpoint, transport);
assert.equal(a.current, null);
assert.equal(a.observationTransport(), null);
const identity = await a.signInWithPassword("owner", password);
assert.deepEqual(identity, {
  site: "orun-main", actor: "owner", username: "owner",
  installation: namespace, expiresAt: identity.expiresAt,
});
assert.equal(logins, 1);
assert.deepEqual(a.observationTransport(), {
  url: endpoint, token: credential,
});
const fetched = await a.readSharedChanges(0);
assert.deepEqual(fetched.changes, []);
await a.signOut();
assert.equal(logouts, 1);
assert.equal(a.observationTransport(), null);
// New installation / fresh app process: username/password alone.
const newPhone = new OwnerAccountClient(endpoint, transport);
assert.equal((await newPhone.signInWithPassword("owner", password)).installation,
             namespace);
assert.equal(logins, 2);
await newPhone.signOut();

const denied = new OwnerAccountClient(endpoint,
  (async () => new Response('{"error":"invalid_credentials"}', {
    status: 401, headers: { "Content-Type": "application/json" },
  })) as typeof fetch);
await assert.rejects(
  () => denied.signInWithPassword("owner", password), /yanlış/);
assert.equal(denied.current, null);
assert.equal(denied.observationTransport(), null);
assert.throws(
  () => new OwnerAccountClient("http://example.test:8090", transport),
  /HTTPS/);
console.log("ORUN_PASSWORD_ONLY_ANDROID_CLIENT_HOST_PASS");
