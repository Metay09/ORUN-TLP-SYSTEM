// ORUN S6B pilot account runtime: holds account bearer in process memory only.
// Navigating between app screens does not create another auth session.
// No localStorage, preferences, SQLite, browser cache or URL credential writes.
// Server positions sync automatically while signed in: newest position of
// every tracker first, then history from the saved cursor (1. adım).
import {
  OwnerAccountClient, type AccountIdentity,
} from "./ownerAccountClient.ts";
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";
import { ORUN_ACCOUNT_ORIGIN } from "./orunServiceConfig.ts";
import { AccountObservationMirror } from "./accountObservationMirror.ts";
import type { Point } from "../core/points.ts";
import {
  AccountSharedMirror, type SharedMirrorSnapshot, type MirrorPullResult,
} from "./accountSharedMirror.ts";

let client: OwnerAccountClient | null = null;
let busy = false;
let sqliteMirror: AccountSharedMirror | null = null;
let observationMirror: AccountObservationMirror | null = null;
let observationsChanged: (() => void) | null = null;
let observationPrepareKeys: (() => Promise<void>) | null = null;

/** Registered only by Controller after its existing EdgeCore SQLite is ready.
 * This is an opt-in hook; no table is created and no sync starts on boot.
 */
export function attachPilotAccountSqliteMirror(mirror: AccountSharedMirror): void {
  sqliteMirror = mirror;
}

/** Registered by Controller; mirror uses the same serialized SQLite DB but
 * writes ONLY into the account observation tables, never Gateway ACCEPT.
 */
export function attachPilotAccountObservationMirror(
  mirror: AccountObservationMirror, onChanged: () => void,
  prepareKeys: () => Promise<void>,
): void {
  observationMirror = mirror;
  observationsChanged = onChanged;
  observationPrepareKeys = prepareKeys;
  // Signed in before the phone ledger was ready: start the first sync now.
  if (autoTimer !== null) backgroundSync();
}

/** Only active account's verified remote records; logout reveals none. */
export async function pilotAccountObservationPoints(): Promise<Point[]> {
  if (!client || !pilotAccountStatus() || !observationMirror) return [];
  return observationMirror.points(client);
}

export interface ObservationSyncResult {
  /** Trackers whose newest measured position was checked. */
  latest: number;
  /** History records verified in this run. */
  history: number;
  /** True only if the run stopped early (account change, time limit). */
  more: boolean;
  at: number;
}
export interface ObservationSyncState {
  running: boolean;
  last: ObservationSyncResult | null;
  error: string | null;
}

const AUTO_SYNC_MS = 60_000;
const MAX_ROUNDS = 50;          // 50 x 64 pages x 64 records per run
let syncRun: Promise<ObservationSyncResult> | null = null;
let syncState: ObservationSyncState = { running: false, last: null, error: null };
let autoTimer: ReturnType<typeof setInterval> | null = null;
let onVisible: (() => void) | null = null;
const syncListeners = new Set<() => void>();

function setSync(next: Partial<ObservationSyncState>): void {
  syncState = { ...syncState, ...next };
  for (const fn of syncListeners) fn();
}
export function pilotObservationSyncState(): ObservationSyncState { return syncState; }
export function onPilotObservationSync(fn: () => void): () => void {
  syncListeners.add(fn);
  return () => { syncListeners.delete(fn); };
}

/** Newest position of every tracker first, then history from the saved
 * cursor until the server has nothing newer. One run at a time; a second
 * caller joins the running one. Remote AEAD evidence is never a local
 * phone delivery, even after durable mirror COMMIT. */
export function pilotAccountSync(): Promise<ObservationSyncResult> {
  if (syncRun) return syncRun;
  const run = (async (): Promise<ObservationSyncResult> => {
    const account = client;
    const mirror = observationMirror;
    if (!account || !pilotAccountStatus())
      throw new Error("Önce hesabınla giriş yapmalısın.");
    if (!mirror || !observationPrepareKeys)
      throw new Error("Telefonun SQLite defteri henüz hazır değil.");
    // Keys arrive through the protected account relay. Do not assume the
    // phone has per-epoch read keys merely because it can sign in.
    await observationPrepareKeys();
    const same = () => client === account && !!pilotAccountStatus();
    if (!same()) throw new Error("Hesap değişti; sunucu konumları alınmadı.");
    const latest = await mirror.pullLatest(account);
    if (latest.added) observationsChanged?.();
    let history = 0, more = true;
    for (let round = 0; round < MAX_ROUNDS && more && same(); round++) {
      const r = await mirror.pullAvailable(account);
      history += r.received;
      more = r.more;
      if (r.received) observationsChanged?.();
    }
    return { latest: latest.received, history, more, at: Date.now() };
  })();
  syncRun = run;
  setSync({ running: true });
  run.then(
    last => setSync({ running: false, last, error: null }),
    e => setSync({ running: false, error: e instanceof Error ? e.message : String(e) }),
  ).finally(() => { if (syncRun === run) syncRun = null; });
  return run;
}

/** Kept for callers of the old manual button. */
export const pilotAccountDownloadObservations = pilotAccountSync;

function backgroundSync(): void {
  if (!client || !pilotAccountStatus() || !observationMirror) return;
  if (typeof document !== "undefined" && document.visibilityState === "hidden") return;
  void pilotAccountSync().catch(() => { /* shown via pilotObservationSyncState */ });
}

function startAutoSync(): void {
  stopAutoSync();
  backgroundSync();
  autoTimer = setInterval(backgroundSync, AUTO_SYNC_MS);
  if (typeof document !== "undefined") {
    onVisible = () => { if (document.visibilityState === "visible") backgroundSync(); };
    document.addEventListener("visibilitychange", onVisible);
  }
}

function stopAutoSync(): void {
  if (autoTimer !== null) clearInterval(autoTimer);
  autoTimer = null;
  if (onVisible && typeof document !== "undefined")
    document.removeEventListener("visibilitychange", onVisible);
  onVisible = null;
}

function readyAccountMirror(): { account: OwnerAccountClient; mirror: AccountSharedMirror } {
  const account = client;
  const mirror = sqliteMirror;
  if (!account || !pilotAccountStatus())
    throw new Error("Önce hesabınla giriş yapmalısın.");
  if (!mirror)
    throw new Error("Telefonun SQLite veritabanı henüz hazır değil.");
  return { account, mirror };
}

/** No network: reads the already committed canonical draft mirror only. */
export async function pilotAccountStoredSnapshot(): Promise<SharedMirrorSnapshot> {
  const { account, mirror } = readyAccountMirror();
  await mirror.init();
  return mirror.snapshot(account);
}

/** Explicit, bounded, read-only authenticated download to durable SQLite.
 * Never touches original local owner zone editor drafts or sends a command.
 */
export async function pilotAccountDownloadShared(): Promise<MirrorPullResult> {
  const { account, mirror } = readyAccountMirror();
  await mirror.init();
  return mirror.pullAvailable(account);
}


/** Internal-only transport for phone ledger upload through account gateway. */
export function pilotAccountUploadTransport(): { url: string; token: string } | null {
  return client?.observationTransport() ?? null;
}

export function pilotAccountStatus(): AccountIdentity | null {
  const identity = client?.current ?? null;
  if (!identity) return null;
  if (identity.expiresAt <= Date.now() / 1000) {
    // Expired 12h token is never presented as a live login.
    client = null;
    return null;
  }
  return identity;
}

/** Password-only account login. No phone enrollment, keys, or PINs. */
export async function pilotPasswordLogin(
  username: string, password: string,
): Promise<AccountIdentity> {
  if (busy) throw new Error("Hesap oturum işlemi zaten devam ediyor.");
  busy = true;
  try {
    if (client) {
      const old = client;
      client = null;
      await old.signOut();
    }
    const next = new OwnerAccountClient(ORUN_ACCOUNT_ORIGIN);
    const user = await next.signInWithPassword(username, password);
    client = next;
    observationsChanged?.();
    startAutoSync();
    return user;
  } finally {
    busy = false;
  }
}

export async function pilotAccountLogin(
  device: ActiveMobileSession, username: string, password: string,
): Promise<AccountIdentity> {
  if (busy) throw new Error("Hesap oturum işlemi zaten devam ediyor.");
  busy = true;
  try {
    // Do not leave an old login active on the server when switching user.
    if (client) {
      const old = client;
      client = null;
      // Failure to revoke old session must NOT issue a second login.
      await old.signOut();
    }
    const next = new OwnerAccountClient(device.origin);
    const user = await next.signIn(device, username, password);
    client = next;
    observationsChanged?.();
    startAutoSync();
    return user;
  } finally {
    busy = false;
  }
}

export async function pilotAccountReadShared(): Promise<{
  received: number;
  cursor: number;
  more: boolean;
}> {
  if (!client) throw new Error("Önce hesabınla giriş yapmalısın.");
  const result = await client.readSharedChanges(0);
  if (!Array.isArray(result.changes) ||
      typeof result.cursor !== "number" ||
      typeof result.has_more !== "boolean") {
    throw new Error("Sunucunun ortak veri yanıtı geçersiz.");
  }
  // This is ONLY the first page and does not store/sync changes.
  return {
    received: result.changes.length,
    cursor: result.cursor,
    more: result.has_more,
  };
}

export async function pilotAccountLogout(): Promise<void> {
  if (busy) throw new Error("Hesap oturum işlemi devam ediyor.");
  const prior = client;
  client = null;
  stopAutoSync();
  setSync({ running: false, last: null, error: null });
  observationsChanged?.();
  if (prior) await prior.signOut();
}
