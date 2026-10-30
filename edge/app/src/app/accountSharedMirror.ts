// ORUN S6C — durable, account-scoped shared-data mirror (READ ONLY).
//
// Reuses the existing SQLite Db interface and the validated OfflineSyncStore
// canonical-change rules. This class never opens a new observation DB, never
// touches the owner's original localStorage zone/target drafts, never stages
// outgoing operations and never claims a Tracker applied a new boundary.
//
// Caller MUST give the same existing Db connection that owns its transactions;
// integration with Controller's exclusive DB access is a separate release gate.
// Mirrored zone coordinates remain plaintext in the existing app SQLite DB;
// Android backup / at-rest encryption requires review before production use.
import type { Db } from "../core/db.ts";
import {
  OfflineSyncStore, type SyncChange, type SyncResource,
} from "./offlineSync.ts";
import type { OwnerAccountClient, AccountIdentity } from "./ownerAccountClient.ts";

const TABLE = "orun_account_shared_mirror_v1";
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const MAX_PER_PAGE = 200;
const MAX_PAGES = 5;
const MAX_STATE_BYTES = 2 * 1024 * 1024;

export interface SharedMirrorSnapshot {
  site: string;
  actor: string;
  installation: string;
  cursor: number;
  /** Only canonical server records, not pending local edits or Tracker APPLIED. */
  resources: Record<string, SyncResource>;
  zones: number;
  groups: number;
  targets: number;
}
export interface MirrorPullResult {
  received: number;
  cursor: number;
  more: boolean;
  pages: number;
  zones: number;
  groups: number;
  targets: number;
}
interface Stored {
  generation: number;
  serialized: string | null;
}
function checkedActor(actor: AccountIdentity | null): AccountIdentity {
  if (!actor || !ID.test(actor.site) || !ID.test(actor.actor) ||
      !ID.test(actor.installation) || !ID.test(actor.username) ||
      !Number.isSafeInteger(actor.expiresAt) ||
      actor.expiresAt <= Date.now() / 1000)
    throw new Error("Önce geçerli hesabınla giriş yapmalısın.");
  return actor;
}
function sameIdentity(a: AccountIdentity | null, b: AccountIdentity): boolean {
  return Boolean(a && a.site === b.site && a.actor === b.actor &&
    a.installation === b.installation && a.username === b.username &&
    a.expiresAt === b.expiresAt && a.expiresAt > Date.now() / 1000);
}
function jsonBytes(value: string): number {
  return new TextEncoder().encode(value).length;
}
function canonicalRecordCount(resources: Record<string, SyncResource>): {
  zones: number; groups: number; targets: number;
} {
  const count = { zones: 0, groups: 0, targets: 0 };
  for (const [key, record] of Object.entries(resources)) {
    if (record.deleted) continue;
    if (key.startsWith("zone:")) count.zones++;
    if (key.startsWith("group:")) count.groups++;
    if (key.startsWith("zone_target:")) count.targets++;
  }
  return count;
}

export class AccountSharedMirror {
  private busy = false;
  private ready = false;
  private readonly db: Db;

  constructor(db: Db) {
    this.db = db;
  }

  /** Adds one independent table to the existing DB; no other schema changed. */
  async init(): Promise<void> {
    await this.db.execute(`CREATE TABLE IF NOT EXISTS ${TABLE} (
      site_id TEXT NOT NULL,
      actor_id TEXT NOT NULL,
      installation_id TEXT NOT NULL,
      generation INTEGER NOT NULL CHECK(generation>=1),
      state_json TEXT NOT NULL,
      PRIMARY KEY(site_id,actor_id,installation_id)
    )`);
    this.ready = true;
  }

  private requireReady(): void {
    if (!this.ready) throw new Error("Kalıcı ortak veri deposu hazır değil.");
  }
  private params(account: AccountIdentity): string[] {
    return [account.site, account.actor, account.installation];
  }
  private async read(account: AccountIdentity): Promise<Stored> {
    this.requireReady();
    const rows = await this.db.query(
      `SELECT generation,state_json FROM ${TABLE}
       WHERE site_id=? AND actor_id=? AND installation_id=?`,
      this.params(account));
    if (rows.length > 1) throw new Error("Ortak veri kimliği çakışıyor.");
    if (!rows.length) return { generation: 0, serialized: null };
    const row = rows[0];
    if (typeof row.state_json !== "string" ||
        jsonBytes(row.state_json) > MAX_STATE_BYTES ||
        !Number.isSafeInteger(row.generation) ||
        Number(row.generation) < 1)
      throw new Error("Ortak veri deposu bozuk; veri korunarak işlem durduruldu.");
    return { generation: Number(row.generation), serialized: row.state_json };
  }

  private decode(account: AccountIdentity, stored: Stored): OfflineSyncStore {
    let raw = stored.serialized;
    const storage = {
      getItem: (_key: string): string | null => raw,
      setItem: (_key: string, next: string): void => { raw = next; },
    };
    const mirror = new OfflineSyncStore(
      storage, "sqlite-account-mirror", account.site, account.installation);
    if (mirror.warning)
      throw new Error("Ortak veri deposu doğrulanamadı; mevcut kayıtlar korundu.");
    const state = mirror.get();
    if (state.outbox.length || state.problems.length || state.legacyStaged)
      throw new Error("Okuma aynasında beklenmeyen gönderim verisi var.");
    return mirror;
  }

  /** Does not require network, but DOES require an active account session:
   * one user's cached data must never appear under another login. */
  async snapshot(client: OwnerAccountClient): Promise<SharedMirrorSnapshot> {
    const actor = checkedActor(client.current);
    const mirror = this.decode(actor, await this.read(actor));
    if (!sameIdentity(client.current, actor))
      throw new Error("Hesap oturumu değişti; ortak veri gösterilmedi.");
    const state = mirror.get();
    return { site: actor.site, actor: actor.actor,
      installation: actor.installation, cursor: state.cursor,
      resources: state.resources, ...canonicalRecordCount(state.resources) };
  }

  /** One authenticated server page, one durable SQLite transaction.
   * On invalid response, storage failure or account change, cursor and
   * resources are never advanced. The account session is kept in memory,
   * not in the SQLite state. */
  async pullPage(client: OwnerAccountClient): Promise<{
    received: number; cursor: number; more: boolean;
  }> {
    if (this.busy) throw new Error("Ortak veri aktarımı devam ediyor.");
    this.busy = true;
    try {
      const actor = checkedActor(client.current);
      const prior = await this.read(actor);
      const mirror = this.decode(actor, prior);
      const previousCursor = mirror.get().cursor;
      const response = await client.readSharedChanges(previousCursor);
      if (!sameIdentity(client.current, actor))
        throw new Error("Hesap değişti; indirilen veriler kaydedilmedi.");
      if (!Array.isArray(response.changes) ||
          response.changes.length > MAX_PER_PAGE ||
          typeof response.has_more !== "boolean" ||
          typeof response.cursor !== "number" ||
          !Number.isSafeInteger(response.cursor) ||
          response.cursor < previousCursor ||
          (response.has_more && response.changes.length === 0))
        throw new Error("Sunucu sayfa yanıtı hatalı; imleç korunuyor.");
      // OfflineSyncStore independently checks monotonically increasing
      // sequences, revisions, deletes and same-revision divergence.
      mirror.applyPull(response.changes as SyncChange[], response.cursor);
      const serialized = JSON.stringify(mirror.get());
      if (jsonBytes(serialized) > MAX_STATE_BYTES)
        throw new Error("Kalıcı ortak veri kapasitesi doldu; imleç korunuyor.");
      if (response.cursor !== previousCursor) {
        await this.db.transaction(async () => {
          const current = await this.read(actor);
          if (current.generation !== prior.generation ||
              current.serialized !== prior.serialized)
            throw new Error("Ortak veri başka işlemde değişti; yeniden dene.");
          if (!sameIdentity(client.current, actor))
            throw new Error("Hesap oturumu değişti; veriler kaydedilmedi.");
          if (prior.generation === 0) {
            await this.db.run(
              `INSERT INTO ${TABLE}
               (site_id,actor_id,installation_id,generation,state_json)
               VALUES(?,?,?,?,?)`,
              [...this.params(actor), 1, serialized]);
          } else {
            await this.db.run(
              `UPDATE ${TABLE} SET generation=?,state_json=?
               WHERE site_id=? AND actor_id=? AND installation_id=?`,
              [prior.generation + 1, serialized, ...this.params(actor)]);
          }
          // Logout may happen while awaiting the SQLite write. Check again
          // before returning from the transaction callback so a concurrent
          // logout rolls back the page rather than persisting old-user data.
          if (!sameIdentity(client.current, actor))
            throw new Error("Hesap oturumu değişti; SQLite işlemi geri alındı.");
        });
        const persisted = await this.read(actor);
        if (persisted.serialized !== serialized)
          throw new Error("Yerel SQLite kaydı doğrulanamadı.");
      }
      return { received: response.changes.length,
        cursor: response.cursor, more: response.has_more };
    } finally {
      this.busy = false;
    }
  }

  /** Bounded catch-up; each page remains durable across app restarts. */
  async pullAvailable(client: OwnerAccountClient,
                      maxPages = MAX_PAGES): Promise<MirrorPullResult> {
    if (!Number.isSafeInteger(maxPages) || maxPages < 1 ||
        maxPages > MAX_PAGES)
      throw new Error("Sayfa sınırı geçersiz.");
    let received = 0, more = false, pages = 0;
    for (let i = 0; i < maxPages; i++) {
      const page = await this.pullPage(client);
      received += page.received;
      more = page.more;
      pages++;
      if (!more) break;
    }
    const state = await this.snapshot(client);
    return { received, cursor: state.cursor, more, pages,
      zones: state.zones, groups: state.groups, targets: state.targets };
  }
}
