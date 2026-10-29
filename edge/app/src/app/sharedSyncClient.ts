// ORUN S3B: authenticated shared-data HTTP transport candidate.
//
// NOT CONNECTED to the running app, its existing bench upload token, Android
// lifecycle, or live server. A future Step-3C enrolled session must supply the
// per-installation credential from protected storage. Never log the credential.
// This service transports shared zone/group/target DRAFTS, not Tracker commands.
import {
  OfflineSyncStore, type SyncChange, type SyncOp, type SyncResult,
} from "./offlineSync.ts";

export interface EnrolledSyncSession {
  site: string;
  installation: string;
  /** From protected per-installation storage, never the Edge bench upload key. */
  accessToken: string;
}
export type SyncSessionProvider = () =>
  EnrolledSyncSession | null | Promise<EnrolledSyncSession | null>;

export type SyncFetch = (url: string, init: RequestInit) => Promise<Response>;

export interface SyncRunResult {
  accepted: number;
  moreAvailable: boolean;
  conflicts: number;
  rejected: number;
  received: number;
  cursor: number;
  pending: number;
  needsReview: number;
}
export type SyncFailure =
  | "not_enrolled" | "wrong_installation" | "auth_expired" | "forbidden"
  | "server_unavailable" | "protocol" | "local_storage" | "busy";

export class SharedSyncError extends Error {
  readonly reason: SyncFailure;
  constructor(reason: SyncFailure, message: string) {
    super(message);
    this.name = "SharedSyncError";
    this.reason = reason;
  }
}

const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const REQUEST_TIMEOUT_MS = 15_000;
const MAX_PULL_PAGES = 10;
const RESPONSE_LIMIT = 512 * 1024;
const MAX_POST_BODY = 120 * 1024; // backend limit: 128 KiB

/** HTTPS required except exact loopback for a local-host-only test. No
 * Tailscale/HTTP bypass: TLS and enrollment must be reviewed first. */
export function sharedSyncOrigin(value: string): string {
  let u: URL;
  try { u = new URL(value); }
  catch { throw new SharedSyncError("protocol", "Sunucu adresi geçersiz."); }
  const loopback = u.hostname === "localhost" || u.hostname === "127.0.0.1" ||
    u.hostname === "[::1]";
  if ((u.protocol !== "https:" && !(u.protocol === "http:" && loopback)) ||
      u.username || u.password || u.search || u.hash || u.pathname !== "/") {
    throw new SharedSyncError("protocol", "Paylaşılan veriye güvenli sunucu adresi gerekli.");
  }
  return u.origin;
}

function validSession(x: EnrolledSyncSession | null, store: OfflineSyncStore):
  asserts x is EnrolledSyncSession {
  if (!x || typeof x !== "object" || typeof x.site !== "string" ||
      typeof x.installation !== "string" ||
      !ID.test(x.site) || !ID.test(x.installation) ||
      typeof x.accessToken !== "string" || !TOKEN.test(x.accessToken)) {
    throw new SharedSyncError("not_enrolled", "Önce bu telefonun yetkili kaydı gerekli.");
  }
  const local = store.get();
  if (x.site !== local.site || x.installation !== local.installation) {
    throw new SharedSyncError("wrong_installation",
      "Bu telefonun eşitleme kayıtları başka bir yetkili kuruluma ait.");
  }
}

function validatedResults(raw: unknown, requested: SyncOp[]): SyncResult[] {
  if (!raw || typeof raw !== "object" || !("results" in raw) ||
      !Array.isArray(raw.results) || raw.results.length !== requested.length) {
    throw new SharedSyncError("protocol", "Sunucu işlem sonuçları eksik veya geçersiz.");
  }
  const ids = new Set(requested.map((x) => x.op_id));
  const received = new Set<string>();
  const results: SyncResult[] = [];
  for (const item of raw.results) {
    if (!item || typeof item !== "object" ||
        typeof item.op_id !== "string" || !ids.has(item.op_id) ||
        received.has(item.op_id) ||
        !["ACCEPTED", "CONFLICT", "REJECTED"].includes(item.status)) {
      throw new SharedSyncError("protocol", "Sunucu işlemleri eşleşmiyor.");
    }
    received.add(item.op_id);
    results.push(item as SyncResult);
  }
  return results;
}

function validatedChanges(raw: unknown, previousCursor: number):
  { changes: SyncChange[]; cursor: number; hasMore: boolean } {
  if (!raw || typeof raw !== "object" ||
      !("changes" in raw) || !Array.isArray(raw.changes) ||
      !("cursor" in raw) || typeof raw.cursor !== "number" ||
      !Number.isSafeInteger(raw.cursor) || raw.cursor < previousCursor ||
      !("has_more" in raw) || typeof raw.has_more !== "boolean" ||
      raw.changes.length > 200) {
    throw new SharedSyncError("protocol", "Sunucu değişiklik yanıtı geçersiz.");
  }
  const cursor = raw.cursor as number;
  const changes = raw.changes as SyncChange[];
  if ((!changes.length && cursor !== previousCursor) ||
      (raw.has_more && !changes.length) ||
      (changes.length && changes[changes.length - 1]?.sequence !== cursor)) {
    throw new SharedSyncError("protocol", "Sunucu cursor ilerlemesi tutarsız.");
  }
  return { changes, cursor, hasMore: raw.has_more };
}

export class SharedSyncClient {
  private running = false;
  private readonly origin: string;
  private readonly store: OfflineSyncStore;
  private readonly session: SyncSessionProvider;
  private readonly fetcher: SyncFetch;

  constructor(store: OfflineSyncStore, baseUrl: string,
              session: SyncSessionProvider, fetcher: SyncFetch = fetch) {
    this.store = store;
    this.session = session;
    this.fetcher = fetcher;
    this.origin = sharedSyncOrigin(baseUrl);
  }

  private async request(path: string, token: string,
                        method: "GET" | "POST", body?: unknown): Promise<unknown> {
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), REQUEST_TIMEOUT_MS);
    try {
      const res = await this.fetcher(this.origin + path, {
        method,
        redirect: "manual",
        cache: "no-store",
        credentials: "omit",
        signal: abort.signal,
        headers: {
          Authorization: "Bearer " + token,
          Accept: "application/json",
          ...(method === "POST" ? { "Content-Type": "application/json" } : {}),
        },
        ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      });
      if (res.status === 401)
        throw new SharedSyncError("auth_expired", "Bu telefonun sunucu erişim yetkisi yenilenmeli.");
      if (res.status === 403)
        throw new SharedSyncError("forbidden", "Bu işlem için yetkiniz yok.");
      if (!res.ok)
        throw new SharedSyncError(res.status >= 500 || res.status === 429
          ? "server_unavailable" : "protocol", "Sunucu eşitleme işlemini kabul etmedi.");
      if (!res.headers.get("content-type")?.toLowerCase().includes("application/json"))
        throw new SharedSyncError("protocol", "Sunucu JSON yanıtı vermedi.");
      if (res.headers.get("content-length") &&
          Number(res.headers.get("content-length")) > RESPONSE_LIMIT)
        throw new SharedSyncError("protocol", "Sunucu yanıtı çok büyük.");
      const text = await res.text();
      if (text.length > RESPONSE_LIMIT)
        throw new SharedSyncError("protocol", "Sunucu yanıtı çok büyük.");
      try { return JSON.parse(text) as unknown; }
      catch { throw new SharedSyncError("protocol", "Sunucu yanıtı okunamadı."); }
    } catch (e) {
      if (e instanceof SharedSyncError) throw e;
      throw new SharedSyncError("server_unavailable", "Sunucu bağlantısı kurulamadı; kayıtlar korunuyor.");
    } finally {
      clearTimeout(timer);
    }
  }

  /** Explicit, ONE pass; never auto-starts on app boot.
   *
   * A lost reply may cause the SAME op_id to be resent; backend idempotency
   * reconciles it. A response is persisted BEFORE it is removed from outbox.
   * Pull has a strict page bound; another explicit pass resumes at the cursor.
   */
  async runOnce(): Promise<SyncRunResult> {
    if (this.running) throw new SharedSyncError("busy", "Eşitleme zaten çalışıyor.");
    this.running = true;
    try {
      if (this.store.warning)
        throw new SharedSyncError("local_storage", "Eşitleme kaydı bozuk; veriler korunuyor.");
      const session = await this.session();
      validSession(session, this.store);

      let accepted = 0, conflicts = 0, rejected = 0, received = 0;
      let moreAvailable = false;
      const batch: SyncOp[] = [];
      for (const op of this.store.nextBatch(50)) {
        const proposed = [...batch, op];
        const size = new TextEncoder().encode(JSON.stringify({ ops: proposed })).length;
        if (size > MAX_POST_BODY) {
          if (!batch.length)
            throw new SharedSyncError("protocol", "Tek işlem sunucu paket sınırını aşıyor.");
          break;
        }
        batch.push(op);
      }
      if (batch.length) {
        const raw = await this.request("/api/v1/sync/ops", session.accessToken,
          "POST", { ops: batch });
        const results = validatedResults(raw, batch);
        // The Store validates revisions and commits before deleting requests.
        try { this.store.recordResults(results); }
        catch {
          throw new SharedSyncError("local_storage",
            "Sunucu yanıtı yerel kayıtlara güvenle uygulanamadı; kuyruk korunuyor.");
        }
        accepted = results.filter((r) => r.status === "ACCEPTED").length;
        conflicts = results.filter((r) => r.status === "CONFLICT").length;
        rejected = results.filter((r) => r.status === "REJECTED").length;
      }

      for (let page = 0; page < MAX_PULL_PAGES; page++) {
        const cursor = this.store.get().cursor;
        const response = await this.request(
          "/api/v1/sync?cursor=" + cursor + "&limit=100", session.accessToken, "GET");
        const result = validatedChanges(response, cursor);
        try { this.store.applyPull(result.changes, result.cursor); }
        catch {
          throw new SharedSyncError("local_storage",
            "Sunucu değişiklikleri kaydedilemedi; önceki kayıtlar korunuyor.");
        }
        received += result.changes.length;
        moreAvailable = result.hasMore;
        if (!result.hasMore) break;
      }

      const state = this.store.get();
      return {
        accepted, conflicts, rejected, received, moreAvailable, cursor: state.cursor,
        pending: state.outbox.length, needsReview: state.problems.length,
      };
    } finally {
      this.running = false;
    }
  }
}
