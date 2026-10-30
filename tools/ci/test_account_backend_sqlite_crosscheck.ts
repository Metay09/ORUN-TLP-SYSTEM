// S6E: real Python HTTP responder, real TypeScript client and real Node SQLite.
// This local-only adapter forwards the client's HTTPS *test origin* to a
// 127.0.0.1 Python fixture, without changing any production TLS requirements.
// No real accounts, credentials, services, or Android APK.
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { OwnerAccountClient } from "../../edge/app/src/app/ownerAccountClient.ts";
import { AccountSharedMirror } from
  "../../edge/app/src/app/accountSharedMirror.ts";
import { NodeDb } from "../../edge/app/test/nodeDb.ts";
import type { ActiveMobileSession } from
  "../../edge/app/src/platform/orunMobileSession.ts";

const fixture = JSON.parse(readFileSync(0, "utf8")) as {
  port: number; site: string; installation: string;
  deviceToken: string; password: string;
};
assert.ok(Number.isSafeInteger(fixture.port) &&
          fixture.port > 1024 && fixture.port < 65536);
const origin = "https://fixture.orun.invalid:8443";
const adapter = (async (input: RequestInfo | URL, init?: RequestInit) => {
  const url = new URL(String(input));
  assert.equal(url.origin, origin, "only synthetic HTTPS fixture allowed");
  assert.ok(url.pathname.startsWith("/api/v1/"),
            "only known account and sync API allowed");
  const internal = "http://127.0.0.1:" + fixture.port +
    url.pathname + url.search;
  const response = await fetch(internal, {
    ...init, redirect: "error", credentials: "omit",
  });
  return response;
}) as typeof fetch;
const device: ActiveMobileSession = {
  present: true, origin, installation: fixture.installation,
  site: fixture.site, accessToken: fixture.deviceToken,
  expiresAt: Math.floor(Date.now() / 1000) + 3600,
};
const scratch = mkdtempSync(path.join(tmpdir(), "orun-s6e-sqlite-"));
const sqlite = path.join(scratch, "phone.sqlite");

try {
  // The phone's enrolled device bearer alone CANNOT read shared resources
  // once account login mode is active. The account token is separate.
  const alone = await adapter(
    origin + "/api/v1/sync?cursor=0", {
      headers: { Authorization: "Bearer " + fixture.deviceToken },
    });
  assert.equal(alone.status, 401);

  const account = new OwnerAccountClient(origin, adapter);
  assert.equal(account.current, null);
  await assert.rejects(() => account.readSharedChanges(), /giriş/);
  const identity = await account.signIn(
    device, "ownerlogin", fixture.password);
  assert.equal(identity.site, fixture.site);
  assert.equal(identity.installation, fixture.installation);
  assert.equal(identity.actor, "owner");
  assert.equal("accessToken" in identity, false);

  const db = new NodeDb(sqlite);
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const downloaded = await mirror.pullAvailable(account);
  assert.equal(downloaded.received, 3);
  assert.equal(downloaded.cursor, 3);
  assert.equal(downloaded.more, false);
  assert.equal(downloaded.zones, 1);
  assert.equal(downloaded.groups, 1);
  assert.equal(downloaded.targets, 1);
  const first = await mirror.snapshot(account);
  assert.equal(first.resources["zone:z1"].body?.name, "Kuzey");
  assert.equal(first.resources["group:g1"].body?.name, "Birinci");
  assert.equal(first.resources["zone_target:z1"].deleted, false);
  assert.equal("applied" in first.resources["zone:z1"], false);
  db.raw.close();

  // Closing and reopening a physical temp SQLite file preserves the cursor.
  const reopened = new NodeDb(sqlite);
  const persisted = new AccountSharedMirror(reopened);
  await persisted.init();
  const afterRestart = await persisted.snapshot(account);
  assert.equal(afterRestart.cursor, 3);
  assert.equal(afterRestart.zones, 1);
  const next = await persisted.pullAvailable(account);
  assert.equal(next.received, 0);
  assert.equal(next.cursor, 3);

  await account.signOut();
  assert.equal(account.current, null);
  await assert.rejects(() => persisted.snapshot(account), /giriş/);
  await assert.rejects(() => persisted.pullAvailable(account), /giriş/);
  reopened.raw.close();
  console.log("ORUN_ACCOUNT_REAL_BACKEND_TS_SQLITE_PASS");
} finally {
  rmSync(scratch, {recursive: true, force: true});
}
