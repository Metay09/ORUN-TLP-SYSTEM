// ORUN S6C: actual node:sqlite, account-bound durable pull, fault injection.
// No Android APK, live Backend, tokens, firmware, Tailscale or real data.
import assert from "node:assert/strict";
import { test } from "node:test";
import { NodeDb } from "./nodeDb.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { AccountSharedMirror } from "../src/app/accountSharedMirror.ts";
import { OwnerAccountClient } from "../src/app/ownerAccountClient.ts";
import type { ActiveMobileSession } from "../src/platform/orunMobileSession.ts";

const ORIGIN = "https://example.test:8443";
const PASSWORD = "ORUN-Offline-Test-Password-2026";
const ring = [[376100000, 280500000], [376100000, 280510000],
              [376110000, 280510000], [376110000, 280500000]];
const change = (seq: number, kind: string, id: string, name: string) => ({
  sequence: seq, kind, id, revision: 1, deleted: false,
  body: kind === "zone"
    ? { name, active: true, ring_e7: ring }
    : kind === "group"
      ? { name, device_ids: [] }
      : { zone_id: "z1", scope: "all", device_ids: [], group_ids: [] },
});
type Page = { changes: unknown[]; cursor: number; has_more: boolean };

async function signedIn(site: string, actor: string, char: string,
                        getPage: (cursor: number) => Promise<Page> | Page) {
  const device: ActiveMobileSession = {
    present: true, origin: ORIGIN,
    installation: "edge-" + char.repeat(64),
    site, accessToken: char.repeat(64),
    expiresAt: Math.floor(Date.now() / 1000) + 86400,
  };
  let reads = 0;
  const fetcher = (async (url: string, options: RequestInit) => {
    assert.equal(options.credentials, "omit");
    if (url.endsWith("/api/v1/account/login")) {
      assert.equal((options.headers as Record<string, string>).Authorization,
                   "Bearer " + device.accessToken);
      return new Response(JSON.stringify({
        site, actor, installation: device.installation,
        username: "ownerlogin", account_access_token: "s".repeat(64),
        expires_at: Math.floor(Date.now() / 1000) + 3600,
      }), {status: 200, headers: {"Content-Type":"application/json"}});
    }
    if (url.endsWith("/api/v1/account/logout")) {
      return new Response('{"signed_out":true}', {
        status:200, headers:{"Content-Type":"application/json"}});
    }
    assert.ok(url.startsWith(ORIGIN + "/api/v1/sync?cursor="), url);
    assert.equal((options.headers as Record<string, string>).Authorization,
                 "Bearer " + "s".repeat(64));
    const cursor = Number(new URL(url).searchParams.get("cursor"));
    reads++;
    return new Response(JSON.stringify(await getPage(cursor)), {
      status:200, headers:{"Content-Type":"application/json"}});
  }) as typeof fetch;
  const client = new OwnerAccountClient(ORIGIN, fetcher);
  await client.signIn(device, "ownerlogin", PASSWORD);
  return { client, device, reads: () => reads };
}

test("two-page SQLite mirror survives app object restart without touching originals", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const records = await signedIn("site-a", "owner", "a", (cursor) => {
    if (cursor === 0) return {
      changes: [change(1,"zone","z1","Kuzey")],
      cursor:1, has_more:true,
    };
    if (cursor === 1) return {
      changes: [change(2,"group","g1","Birinci")],
      cursor:2, has_more:false,
    };
    return {changes:[], cursor, has_more:false};
  });
  const result = await mirror.pullAvailable(records.client);
  assert.deepEqual(result, {
    received:2, cursor:2, more:false, pages:2,
    zones:1, groups:1, targets:0,
  });
  const next = new AccountSharedMirror(db);
  await next.init();
  const restored = await next.snapshot(records.client);
  assert.equal(restored.cursor,2);
  assert.equal(restored.resources["zone:z1"].body?.name,"Kuzey");
  assert.equal(restored.resources["group:g1"].body?.name,"Birinci");
  assert.equal("applied" in restored.resources["zone:z1"],false);
  // No legacy zone/target editor or observation ledger database table changed.
  const tables = await db.query(
    "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'orun_%'");
  assert.deepEqual(tables.map(x => x.name),["orun_account_shared_mirror_v1"]);
  assert.equal(records.reads(),2);
});

test("different account/site/installation cannot read each other's mirror", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const alice = await signedIn("site-a","owner","a", (cursor) => ({
    changes: cursor ? [] : [change(1,"zone","z1","PrivateArea")],
    cursor: cursor || 1, has_more:false,
  }));
  const bob = await signedIn("site-b","owner","b", cursor => ({
    changes:[], cursor, has_more:false,
  }));
  const otherActor = await signedIn("site-a","helper","c", cursor => ({
    changes:[], cursor, has_more:false,
  }));
  const otherPhone = await signedIn("site-a","owner","d", cursor => ({
    changes:[], cursor, has_more:false,
  }));
  await mirror.pullPage(alice.client);
  assert.equal((await mirror.snapshot(alice.client)).zones,1);
  for (const other of [bob, otherActor, otherPhone]) {
    assert.equal((await mirror.snapshot(other.client)).cursor,0);
    assert.equal((await mirror.snapshot(other.client)).zones,0);
  }
});

test("rejected same-revision divergence preserves old records and cursor", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const user = await signedIn("site-a","owner","a", cursor => cursor === 0
    ? {changes:[change(1,"zone","z1","Kuzey")],cursor:1,has_more:false}
    : {changes:[{...change(2,"zone","z1","HACK"),
                 revision:1}],cursor:2,has_more:false});
  await mirror.pullPage(user.client);
  await assert.rejects(() => mirror.pullPage(user.client), /farklı içerik/);
  const saved = await mirror.snapshot(user.client);
  assert.equal(saved.cursor,1);
  assert.equal(saved.resources["zone:z1"].body?.name,"Kuzey");
});

test("commit failure rolls back SQLite cursor and allows same page retry", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const user = await signedIn("site-a","owner","a", () => ({
    changes:[change(1,"zone","z1","Kuzey")],
    cursor:1,has_more:false,
  }));
  db.failNextCommit = true;
  await assert.rejects(() => mirror.pullPage(user.client), /injected/);
  assert.equal((await mirror.snapshot(user.client)).cursor,0);
  const success = await mirror.pullPage(user.client);
  assert.equal(success.cursor,1);
  assert.equal((await mirror.snapshot(user.client)).zones,1);
});

test("malformed response and corrupted stored mirror fail closed without reset", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  const user = await signedIn("site-a","owner","a", () => ({
    changes:[], cursor:99,has_more:false,
  }));
  await assert.rejects(() => mirror.pullPage(user.client), /cursor|imleç/);
  assert.equal((await mirror.snapshot(user.client)).cursor,0);
  await db.run(
    "INSERT INTO orun_account_shared_mirror_v1 VALUES(?,?,?,?,?)",
    ["site-a","owner","edge-"+"a".repeat(64),1,"{BROKEN"]);
  await assert.rejects(() => mirror.snapshot(user.client), /doğrulanamadı/);
  await assert.rejects(() => mirror.pullPage(user.client), /doğrulanamadı/);
  const rows = await db.query(
    "SELECT state_json FROM orun_account_shared_mirror_v1");
  assert.equal(rows[0].state_json, "{BROKEN");
});

test("logout during asynchronous HTTP pull never commits stale account data", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  let resolveFetch!: (page: Page) => void;
  let notifyStarted!: () => void;
  const started = new Promise<void>(resolve => { notifyStarted = resolve; });
  const user = await signedIn("site-a","owner","a", async _cursor => {
    notifyStarted();
    return new Promise<Page>(resolve => { resolveFetch = resolve; });
  });
  const pending = mirror.pullPage(user.client);
  await started;
  await user.client.signOut();
  resolveFetch({
    changes:[change(1,"zone","z1","Kuzey")],
    cursor:1,has_more:false,
  });
  await assert.rejects(() => pending, /Hesap değişti/);
  const row = await db.query(
    "SELECT COUNT(*) AS n FROM orun_account_shared_mirror_v1");
  assert.equal(row[0].n,0);
});


test("same EdgeCore SQLite serialization never blocks BLE/USB while HTTP awaits", async () => {
  const db = new NodeDb();
  const core = new EdgeCore(db, { credentials: () => null });
  await core.init();
  let lockedOperations = 0;
  const mirror = new AccountSharedMirror(
    db, job => {
      lockedOperations++;
      return core.exclusive(job);
    });
  // Lazy table creation is explicit; EdgeCore observations remain untouched.
  const before = await db.query(
    "SELECT name FROM sqlite_master WHERE type='table' "
    + "AND name='orun_account_shared_mirror_v1'");
  assert.equal(before.length, 0);
  await mirror.init();
  let notifyStarted!: () => void;
  let releaseResponse!: (page: Page) => void;
  const started = new Promise<void>(resolve => { notifyStarted = resolve; });
  const user = await signedIn("site-a", "owner", "a", async _cursor => {
    notifyStarted();
    return new Promise<Page>(resolve => { releaseResponse = resolve; });
  });
  const waiting = mirror.pullPage(user.client);
  await started;
  // A real EdgeCore event using the very same Db MUST finish before any
  // remote sync response arrives, proving that HTTP did not hold the lock.
  await core.sessionAlive("USB");
  const session = await db.query("SELECT transport FROM sessions");
  assert.equal(session.length, 1);
  assert.equal(session[0].transport, "USB");
  releaseResponse({
    changes: [change(1, "zone", "z1", "Kuzey")],
    cursor: 1, has_more: false,
  });
  assert.equal((await waiting).cursor, 1);
  assert.equal((await mirror.snapshot(user.client)).zones, 1);
  const observationCount = await db.query("SELECT count(*) AS n FROM observations");
  assert.equal(observationCount[0].n, 0);
  assert.ok(lockedOperations >= 4, "SQL init, read and commit must take EdgeCore lock");
});


test("same user re-login within one second cannot commit old HTTP page", async () => {
  const db = new NodeDb();
  const mirror = new AccountSharedMirror(db);
  await mirror.init();
  let notifyStarted!: () => void;
  let releasePage!: (value: Page) => void;
  const started = new Promise<void>(resolve => { notifyStarted = resolve; });
  const owner = await signedIn("site-a", "owner", "a", async _cursor => {
    notifyStarted();
    return new Promise<Page>(resolve => { releasePage = resolve; });
  });
  const original = owner.client.current;
  const beforeEpoch = owner.client.sessionEpoch;
  const inFlight = mirror.pullPage(owner.client);
  await started;
  await owner.client.signOut();
  await owner.client.signIn(
    owner.device, "ownerlogin", PASSWORD);
  assert.equal(owner.client.current?.site, original?.site);
  assert.equal(owner.client.current?.installation, original?.installation);
  assert.notEqual(owner.client.sessionEpoch, beforeEpoch);
  // Even if the identity and expiry happen to match after re-login,
  // the first HTTP request is bound to a different account session.
  releasePage({
    changes: [change(1, "zone", "z1", "STALE")],
    cursor: 1, has_more: false,
  });
  await assert.rejects(() => inFlight, /oturumu değişti/);
  const state = await mirror.snapshot(owner.client);
  assert.equal(state.cursor, 0);
  assert.equal(state.zones, 0);
});
