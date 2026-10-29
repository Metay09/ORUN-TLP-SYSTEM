// ORUN development-only cleanup: verified *observations* held by this phone.
//
// Do NOT clear application storage/SQLite wholesale: asset assignments,
// pasture zones, targets, read keys, server URL and installation identity
// must survive. No Gateway, Tracker or Backend mutation is performed.
//
// This is deliberately NOT automatic and cannot be called from start()/sync.
// The controller separately requires explicit confirmation, disconnected
// BLE/USB and idle network upload, then serializes this transaction with
// all EdgeCore ingest/accept operations.
import type { Db } from "./db.ts";

export interface ObservationCleanupResult {
  deletedObservations: number;
  deletedConflicts: number;
  deletedReceipts: number;
  deletedUploads: number;
  deletedSessions: number;
}

const COUNT_SQL = {
  observations: "SELECT count(*) AS n FROM observations",
  conflicts: "SELECT count(*) AS n FROM conflicts",
  receipts: "SELECT count(*) AS n FROM receipts",
  uploads: "SELECT count(*) AS n FROM uploads",
  sessions: "SELECT count(*) AS n FROM sessions",
} as const;

async function count(db: Db, table: keyof typeof COUNT_SQL): Promise<number> {
  const rows = await db.query(COUNT_SQL[table]);
  const value = Number(rows[0]?.n);
  if (!Number.isSafeInteger(value) || value < 0)
    throw new Error("Yerel kayıt sayısı doğrulanamadı; hiçbir şey temizlenmedi.");
  return value;
}

export async function cleanDevelopmentObservations(
  db: Db, expectedObservations: number, confirmation: string,
): Promise<ObservationCleanupResult> {
  if (confirmation !== "SİL" || !Number.isSafeInteger(expectedObservations) ||
      expectedObservations < 0) {
    throw new Error("Geliştirme kaydı temizliği açık onay gerektirir.");
  }

  // No committed row may be silently discarded if the backend has not
  // acknowledged it. This applies even when the owner calls records tests.
  return db.transaction(async () => {
    const n = await count(db, "observations");
    if (n !== expectedObservations)
      throw new Error("Kayıt sayısı değişti; temizlik iptal edildi.");
    const unsent = await db.query(
      "SELECT count(*) AS n FROM observations o " +
      "LEFT JOIN uploads u ON u.device_id=o.device_id AND " +
      "u.incarnation=o.incarnation AND u.sequence=o.sequence " +
      "WHERE u.state IS NULL",
    );
    const pending = Number(unsent[0]?.n);
    if (!Number.isSafeInteger(pending) || pending > 0 || pending < 0)
      throw new Error("Henüz sunucu onayı olmayan kayıtlar var; hiçbir kayıt silinmedi.");

    const result: ObservationCleanupResult = {
      deletedObservations: n,
      deletedConflicts: await count(db, "conflicts"),
      deletedReceipts: await count(db, "receipts"),
      deletedUploads: await count(db, "uploads"),
      deletedSessions: await count(db, "sessions"),
    };
    await db.run("DELETE FROM uploads");
    await db.run("DELETE FROM receipts");
    await db.run("DELETE FROM conflicts");
    await db.run("DELETE FROM sessions");
    await db.run("DELETE FROM observations");
    // These keys describe historical session uploads, not permanent
    // installation identity. Keep meta.edge_id and all other meta values.
    await db.run("DELETE FROM meta WHERE key IN ('sessions_from','sessions_sent')");
    return result;
  });
}
