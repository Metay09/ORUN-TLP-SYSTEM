// ORUN S6D host-only account runtime + SQLite wiring test.
// No real owner credential; no Android, Gateway, server or live DB mutations.
import assert from "node:assert/strict";
import { NodeDb } from "../../edge/app/test/nodeDb.ts";
import { EdgeCore } from "../../edge/app/src/core/edge.ts";
import { AccountSharedMirror } from
  "../../edge/app/src/app/accountSharedMirror.ts";
import {
  attachPilotAccountSqliteMirror, pilotAccountLogin, pilotAccountLogout,
  pilotAccountDownloadShared, pilotAccountStoredSnapshot, pilotAccountStatus,
} from "../../edge/app/src/app/ownerAccountPilotRuntime.ts";
import type { ActiveMobileSession } from
  "../../edge/app/src/platform/orunMobileSession.ts";

const phone: ActiveMobileSession = {
  present: true, site: "orun-main", origin: "https://example.test:8443",
  installation: "edge-" + "a".repeat(64),
  accessToken: "d".repeat(64),
  expiresAt: Math.floor(Date.now() / 1000) + 86400,
};
const ring = [[376100000,280500000],[376100000,280510000],
              [376110000,280510000],[376110000,280500000]];
const previous = globalThis.fetch;
let fetches = 0;
globalThis.fetch = (async (url: RequestInfo | URL, init?: RequestInit) => {
  const str = String(url);
  if (str.endsWith("/api/v1/account/login")) {
    assert.equal((init?.headers as Record<string,string>).Authorization,
                 "Bearer " + phone.accessToken);
    return new Response(JSON.stringify({
      account_access_token: "s".repeat(64),
      site:"orun-main", actor:"owner", username:"ownerlogin",
      installation:phone.installation,
      expires_at:Math.floor(Date.now()/1000)+3600,
    }), {status:200,headers:{"Content-Type":"application/json"}});
  }
  if (str.endsWith("/api/v1/account/logout")) {
    return new Response('{"signed_out":true}', {status:200,
      headers:{"Content-Type":"application/json"}});
  }
  assert.equal(str, "https://example.test:8443/api/v1/sync?cursor=" +
                    (fetches === 0 ? "0" : "1"));
  assert.equal((init?.headers as Record<string,string>).Authorization,
               "Bearer " + "s".repeat(64));
  fetches++;
  return new Response(JSON.stringify(fetches === 1 ? {
    changes:[{sequence:1,kind:"zone",id:"z1",revision:1,deleted:false,
      body:{name:"Kuzey",active:true,ring_e7:ring}}],
    cursor:1,has_more:false,
  } : {changes:[],cursor:1,has_more:false}), {
    status:200,headers:{"Content-Type":"application/json"},
  });
}) as typeof fetch;

try {
  const db = new NodeDb();
  const core = new EdgeCore(db, { credentials: () => null });
  await core.init();
  attachPilotAccountSqliteMirror(new AccountSharedMirror(
    db, job => core.exclusive(job)));
  const tablesBefore = await db.query(
    "SELECT name FROM sqlite_master WHERE name='orun_account_shared_mirror_v1'");
  assert.equal(tablesBefore.length, 0,
    "no schema mutation simply because Controller registers account mirror");
  assert.equal(pilotAccountStatus(), null);
  await assert.rejects(() => pilotAccountStoredSnapshot(), /giriş/);
  await pilotAccountLogin(phone, "ownerlogin", "A-Unique-Test-Password-2026");
  const initiallyEmpty = await pilotAccountStoredSnapshot();
  assert.equal(initiallyEmpty.zones, 0);
  assert.equal(initiallyEmpty.cursor, 0);
  const downloaded = await pilotAccountDownloadShared();
  assert.equal(downloaded.received, 1);
  assert.equal(downloaded.zones, 1);
  assert.equal(downloaded.cursor, 1);
  assert.equal(downloaded.more, false);
  // Another mirror object, representing Settings navigation and app restart,
  // reads the SAME existing SQLite database. No new observation DB.
  const restored = new AccountSharedMirror(db, job => core.exclusive(job));
  await restored.init();
  const ownerRecord = await restored.snapshot({
    current: pilotAccountStatus(),
  } as never);
  assert.equal(ownerRecord.resources["zone:z1"].body?.name, "Kuzey");
  const again = await pilotAccountDownloadShared();
  assert.equal(again.received, 0);
  assert.equal(again.cursor, 1);
  const records = await db.query("SELECT count(*) AS n FROM observations");
  assert.equal(records[0].n, 0);
  await pilotAccountLogout();
  assert.equal(pilotAccountStatus(), null);
  await assert.rejects(() => pilotAccountStoredSnapshot(), /giriş/);
  await assert.rejects(() => pilotAccountDownloadShared(), /giriş/);
} finally {
  globalThis.fetch = previous;
}
console.log("ORUN_OWNER_ACCOUNT_SQLITE_WIRING_TS_HOST_PASS");
