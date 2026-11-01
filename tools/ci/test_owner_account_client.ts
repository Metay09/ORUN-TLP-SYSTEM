// S6A host-only account client test: no real secrets, APK, server or phone.
import assert from "node:assert/strict";
import { OwnerAccountClient } from
  "../../edge/app/src/app/ownerAccountClient.ts";
import type { ActiveMobileSession } from
  "../../edge/app/src/platform/orunMobileSession.ts";

const phone: ActiveMobileSession = {
  present: true, origin: "https://example.test:8443",
  installation: "edge-" + "a".repeat(64),
  site: "orun-main", accessToken: "d".repeat(64),
  expiresAt: Math.floor(Date.now() / 1000) + 86400,
};
const shortBearer = "a".repeat(64);
let signedIn = 0;
let signedOut = 0;
const stub = (async (url: string, init: RequestInit) => {
  assert.equal(init.credentials, "omit");
  assert.equal(init.redirect, "error");
  if (url.endsWith("/api/v1/account/login")) {
    signedIn++;
    assert.equal((init.headers as Record<string, string>).Authorization,
                 "Bearer " + phone.accessToken);
    const login = JSON.parse(String(init.body));
    assert.equal(login.username, "ownerlogin");
    assert.equal(login.password, "A-Unique-Host-Only-Password");
    return new Response(JSON.stringify({
      account_access_token: shortBearer,
      site: "orun-main", actor: "owner", username: "ownerlogin",
      installation: phone.installation,
      expires_at: Math.floor(Date.now() / 1000) + 43200,
    }), {status: 200, headers: {"Content-Type":"application/json"}});
  }
  if (url.endsWith("/api/v1/account/logout")) {
    signedOut++;
    assert.equal((init.headers as Record<string,string>).Authorization,
                 "Bearer " + shortBearer);
    return new Response('{"signed_out":true}',
      {status:200, headers:{"Content-Type":"application/json"}});
  }
  assert.equal(url, "https://example.test:8443/api/v1/sync?cursor=0");
  assert.equal((init.headers as Record<string,string>).Authorization,
               "Bearer " + shortBearer);
  return new Response(JSON.stringify({
    cursor: 1, changes: [{sequence:1, kind:"zone", id:"z1"}],
    has_more:false,
  }), {status:200, headers:{"Content-Type":"application/json"}});
}) as typeof fetch;

const client = new OwnerAccountClient("https://example.test:8443", stub);
assert.equal(client.current, null);
await assert.rejects(() => client.readSharedChanges(), /giriş/);
const returned = await client.signIn(
  phone, "ownerlogin", "A-Unique-Host-Only-Password");
assert.equal(returned.site, "orun-main");
assert.equal(returned.actor, "owner");
assert.equal("accessToken" in returned, false);
assert.equal(signedIn, 1);
const page = await client.readSharedChanges();
assert.equal(page.cursor, 1);
assert.equal((page.changes as unknown[]).length, 1);
await client.signOut();
assert.equal(client.current, null);
assert.equal(signedOut, 1);
await assert.rejects(() => client.readSharedChanges(), /giriş/);
// The constructor rejects an insecure origin synchronously. Node's
// assert.rejects expects a Promise; use assert.throws for this fail-closed check.
assert.throws(
  () => new OwnerAccountClient("http://example.test:8443", stub),
  /HTTPS/);
const badSite = (async () => new Response(JSON.stringify({
    account_access_token: shortBearer, site:"other-site",
    actor:"owner", username:"ownerlogin",
    installation:phone.installation,
    expires_at:Math.floor(Date.now()/1000)+43200,
}), {status:200, headers:{"Content-Type":"application/json"}})) as typeof fetch;
const rejected = new OwnerAccountClient("https://example.test:8443", badSite);
await assert.rejects(
  () => rejected.signIn(phone,"ownerlogin","A-Unique-Host-Only-Password"),
  /uyuşmuyor/);
assert.equal(rejected.current, null);
const unauthorized = (async () => new Response(
  '{"error":"invalid_credentials"}',
  {status:401,headers:{"Content-Type":"application/json"}})) as typeof fetch;
const wrong = new OwnerAccountClient("https://example.test:8443", unauthorized);
await assert.rejects(
  () => wrong.signIn(phone,"ownerlogin","A-Unique-Host-Only-Password"),
  /geçersiz/);
assert.equal(wrong.current, null);
console.log("ORUN_OWNER_ACCOUNT_CLIENT_TS_HOST_PASS");
