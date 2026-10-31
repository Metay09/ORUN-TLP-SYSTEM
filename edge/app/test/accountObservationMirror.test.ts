// Disposable Node SQLite tests for account-scoped remote observation mirror.
import assert from "node:assert/strict";
import { test } from "node:test";
import { NodeDb } from "./nodeDb.ts";
import { KAT_PERIODIC } from "./fixtures.ts";
import { benchCredentials, openProductFrame } from "../src/core/product.ts";
import { hex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { AccountObservationMirror } from "../src/app/accountObservationMirror.ts";
import type { OwnerAccountClient, AccountIdentity } from "../src/app/ownerAccountClient.ts";
import type { ServerObservationPage } from "../src/app/serverObservationRead.ts";

const frame = KAT_PERIODIC();
const item = openProductFrame(frame, benchCredentials);
const row = {
  cursor: 1, device: hex(item.deviceId), incarnation: hex(item.incarnation),
  sequence: item.sequence, family: 1 as const, frame: hex(frame),
  first_seen_utc: 1793495511, rssi: -90, snr: 7,
};

function fake(site: string, getPage: (after: number) => ServerObservationPage) {
  const user: AccountIdentity = {
    site, actor: "owner", installation: "account-" + site,
    username: "ownerlogin", expiresAt: Math.floor(Date.now() / 1000) + 3600,
  };
  let epoch = 0;
  let active: AccountIdentity | null = user;
  return {
    current: user,
    get sessionEpoch() { return epoch; },
    readServerObservations: async (after: number) => getPage(after),
    logout: () => { epoch++; active = null; },
    get authenticated() { return active !== null; },
  } as unknown as OwnerAccountClient;
}

async function setup() {
  const db = new NodeDb();
  const core = new EdgeCore(db, { credentials: benchCredentials });
  await core.init();
  const mirror = new AccountObservationMirror(db, benchCredentials, task => core.exclusive(task));
  return { db, core, mirror };
}

test("verified server frame stays in account mirror; local ledger and Gateway ACK remain untouched", async () => {
  const { db, core, mirror } = await setup();
  const a = fake("site-a", after => after === 0
    ? { records: [row], cursor: 1, has_more: false }
    : { records: [], cursor: after, has_more: false });
  const done = await mirror.pullAvailable(a);
  assert.equal(done.received, 1);
  assert.equal(done.cursor, 1);
  assert.equal(await core.ledger.recordCount(), 0);
  assert.equal(core.nextAcceptMessage(), null);
  assert.equal((await mirror.points(a)).length, 1);
  assert.equal((await mirror.points(a))[0].delivery, "server");
  const b = fake("site-b", after => ({ records: [], cursor: after, has_more: false }));
  assert.deepEqual(await mirror.points(b), []);
  assert.equal(Number((await db.query("SELECT count(*) AS n FROM orun_account_observation_records_v1"))[0].n), 1);
});

test("tamper and failed SQLite COMMIT preserve cursor and do not fabricate delivery", async () => {
  const { db, core, mirror } = await setup();
  const corrupt = row.frame.slice(0, -2) + (row.frame.endsWith("00") ? "01" : "00");
  const bad = fake("site-a", () => ({ records: [{ ...row, frame: corrupt }], cursor: 1, has_more: false }));
  await assert.rejects(mirror.pullPage(bad));
  const valid = fake("site-a", () => ({ records: [row], cursor: 1, has_more: false }));
  db.failNextCommit = true;
  await assert.rejects(mirror.pullPage(valid), /injected/);
  assert.deepEqual(await db.query("SELECT * FROM orun_account_observation_records_v1"), []);
  assert.deepEqual(await db.query("SELECT * FROM orun_account_observation_cursors_v1"), []);
  assert.equal((await mirror.pullPage(valid)).cursor, 1);
  assert.equal(await core.ledger.recordCount(), 0);
  assert.equal(core.nextAcceptMessage(), null);
});
