// S6B host-only pilot session lifecycle. No real account/token or network.
import assert from "node:assert/strict";
import {
  pilotAccountLogin, pilotAccountLogout, pilotAccountStatus,
  pilotAccountReadShared,
} from "../../edge/app/src/app/ownerAccountPilotRuntime.ts";
import type { ActiveMobileSession } from
  "../../edge/app/src/platform/orunMobileSession.ts";

const phone: ActiveMobileSession = {
  present: true, origin: "https://example.test:8443",
  installation: "edge-" + "a".repeat(64),
  site: "orun-main", accessToken: "d".repeat(64),
  expiresAt: Math.floor(Date.now() / 1000) + 86400,
};
let logins = 0;
let logouts = 0;
let reads = 0;
const originalFetch = globalThis.fetch;
globalThis.fetch = (async (url: RequestInfo | URL, init?: RequestInit) => {
  assert.equal(typeof url, "string");
  assert.equal(init?.credentials, "omit");
  const target = String(url);
  if (target.endsWith("/api/v1/account/login")) {
    logins++;
    assert.equal((init?.headers as Record<string, string>).Authorization,
                 "Bearer " + phone.accessToken);
    return new Response(JSON.stringify({
      account_access_token: "s".repeat(64),
      username: "ownerlogin", actor: "owner",
      site: phone.site, installation: phone.installation,
      expires_at: Math.floor(Date.now() / 1000) + 3600,
    }), { status: 200, headers: { "Content-Type": "application/json" } });
  }
  if (target.endsWith("/api/v1/account/logout")) {
    logouts++;
    assert.equal((init?.headers as Record<string, string>).Authorization,
                 "Bearer " + "s".repeat(64));
    return new Response('{"signed_out":true}', {
      status: 200, headers: { "Content-Type": "application/json" },
    });
  }
  assert.equal(target, "https://example.test:8443/api/v1/sync?cursor=0");
  reads++;
  return new Response(JSON.stringify({
    changes: [{ sequence: 1, kind: "zone", id: "z1" }],
    cursor: 1, has_more: true,
  }), { status: 200, headers: { "Content-Type": "application/json" } });
}) as typeof fetch;

try {
  assert.equal(pilotAccountStatus(), null);
  await assert.rejects(() => pilotAccountReadShared(), /giriş/);
  const account = await pilotAccountLogin(
    phone, "ownerlogin", "A-Unique-Host-Only-Password");
  assert.equal(account.actor, "owner");
  assert.equal(pilotAccountStatus()?.site, "orun-main");
  assert.equal(logins, 1);
  assert.equal("accessToken" in account, false);
  const firstPage = await pilotAccountReadShared();
  assert.deepEqual(firstPage, { received: 1, cursor: 1, more: true });
  assert.equal(reads, 1);
  await pilotAccountLogout();
  assert.equal(logouts, 1);
  assert.equal(pilotAccountStatus(), null);
  await assert.rejects(() => pilotAccountReadShared(), /giriş/);
} finally {
  globalThis.fetch = originalFetch;
}
console.log("ORUN_ACCOUNT_PILOT_RUNTIME_TS_HOST_PASS");
