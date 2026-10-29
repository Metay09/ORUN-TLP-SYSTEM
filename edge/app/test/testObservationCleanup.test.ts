// Phone-only development cleanup qualification, without Android APK or live DB.
import assert from "node:assert/strict";
import { test } from "node:test";
import { NodeDb } from "./nodeDb.ts";
import { SCHEMA } from "../src/core/db.ts";
import { cleanDevelopmentObservations } from "../src/core/testObservationCleanup.ts";

const DEVICE = "1122334455667788";
const INC = "0102030405060708";
const TEXT = "AA".repeat(48);
const FRAME = "BB".repeat(84);

async function prepared(): Promise<NodeDb> {
  const db = new NodeDb();
  for (const sql of SCHEMA) await db.execute(sql);
  await db.run("INSERT INTO meta VALUES (?,?)", ["edge_id", "phone-abcdef012345"]);
  await db.run("INSERT INTO meta VALUES (?,?)", ["sessions_from", "9"]);
  await db.run("INSERT INTO meta VALUES (?,?)", ["sessions_sent", "stale-batch"]);
  await db.run("INSERT INTO meta VALUES (?,?)", ["other_safe_setting", "preserve"]);
  await db.run("INSERT INTO read_keys VALUES (?,?,?)", [DEVICE, 1, "AA".repeat(16)]);
  await db.run("INSERT INTO sessions(started_utc,alive_utc,transport) VALUES (?,?,?)", [100, 200, "ble"]);
  return db;
}

async function observation(db: NodeDb, sequence: number, uploaded: boolean) {
  await db.run("INSERT INTO observations VALUES (?,?,?,?,?,?)",
    [DEVICE, INC, sequence, 1, TEXT, FRAME]);
  await db.run("INSERT INTO receipts VALUES (?,?,?,?,?,?,?)",
    [DEVICE, INC, sequence, 130 + sequence, -80, 5, "FFEEDDCCBBAA0099"]);
  if (uploaded) await db.run("INSERT INTO uploads VALUES (?,?,?,?,?)",
    [DEVICE, INC, sequence, "STORED", 150 + sequence]);
}

async function count(db: NodeDb, table: string): Promise<number> {
  const r = await db.query(`SELECT count(*) AS n FROM ${table}`);
  return Number(r[0].n);
}

test("cleanup deletes only observation-related rows and keeps keys, edge identity, settings", async () => {
  const db = await prepared();
  await observation(db, 1, true);
  await observation(db, 2, true);
  await db.run("INSERT INTO conflicts VALUES (?,?,?,?,?,?)",
    [DEVICE, INC, 1, 1, "CD".repeat(48), "EF".repeat(84)]);
  const r = await cleanDevelopmentObservations(db, 2, "SİL");
  assert.deepEqual(r, {
    deletedObservations: 2, deletedConflicts: 1, deletedReceipts: 2,
    deletedUploads: 2, deletedSessions: 1,
  });
  for (const t of ["observations", "conflicts", "receipts", "uploads", "sessions"])
    assert.equal(await count(db, t), 0, `${t} retained observations`);
  assert.equal(await count(db, "read_keys"), 1);
  assert.equal((await db.query("SELECT value FROM meta WHERE key='edge_id'"))[0].value,
    "phone-abcdef012345");
  assert.equal((await db.query("SELECT value FROM meta WHERE key='other_safe_setting'"))[0].value,
    "preserve");
  assert.equal((await db.query("SELECT count(*) AS n FROM meta WHERE key IN ('sessions_from','sessions_sent')"))[0].n, 0);
  // A fresh observation after explicit cleanup is accepted by schema.
  await observation(db, 3, false);
  assert.equal(await count(db, "observations"), 1);
});

test("unsent or uncertain observations cannot be erased even if user confirms", async () => {
  const db = await prepared();
  await observation(db, 1, true);
  await observation(db, 2, false);
  await assert.rejects(cleanDevelopmentObservations(db, 2, "SİL"), /sunucu onayı olmayan/);
  assert.equal(await count(db, "observations"), 2);
  assert.equal(await count(db, "uploads"), 1);
  assert.equal(await count(db, "receipts"), 2);
});

test("confirmation is exact and selected record count is rechecked atomically", async () => {
  const db = await prepared();
  await observation(db, 1, true);
  for (const answer of ["", "sil", "SIL", "SİL ", "YES"])
    await assert.rejects(cleanDevelopmentObservations(db, 1, answer), /açık onay/);
  await assert.rejects(cleanDevelopmentObservations(db, 2, "SİL"), /Kayıt sayısı değişti/);
  assert.equal(await count(db, "observations"), 1);
});

test("a failed SQLite COMMIT preserves every table and the session metadata", async () => {
  const db = await prepared();
  await observation(db, 1, true);
  db.failNextCommit = true;
  await assert.rejects(cleanDevelopmentObservations(db, 1, "SİL"), /injected/);
  assert.equal(await count(db, "observations"), 1);
  assert.equal(await count(db, "receipts"), 1);
  assert.equal(await count(db, "uploads"), 1);
  assert.equal(await count(db, "sessions"), 1);
  assert.equal((await db.query("SELECT value FROM meta WHERE key='sessions_sent'"))[0].value,
    "stale-batch");
});
