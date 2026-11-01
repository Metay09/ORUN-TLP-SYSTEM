// ORUN S6B pilot account runtime: holds account bearer in process memory only.
// Navigating between app screens does not create another auth session.
// No localStorage, preferences, SQLite, browser cache or URL credential writes.
// This runtime does not yet automatically sync herd, zones or device commands.
import {
  OwnerAccountClient, type AccountIdentity,
} from "./ownerAccountClient.ts";
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";
import { ORUN_ACCOUNT_ORIGIN } from "./orunServiceConfig.ts";
import { AccountObservationMirror, type ObservationMirrorResult } from "./accountObservationMirror.ts";
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
}

/** Only active account's verified remote records; logout reveals none. */
export async function pilotAccountObservationPoints(): Promise<Point[]> {
  if (!client || !pilotAccountStatus() || !observationMirror) return [];
  return observationMirror.points(client);
}

/** Manual user action, bounded 4x64 records per request. Remote AEAD evidence
 * is not considered a local phone delivery, even after durable mirror COMMIT.
 */
export async function pilotAccountDownloadObservations(): Promise<ObservationMirrorResult> {
  const account = client;
  const mirror = observationMirror;
  if (!account || !pilotAccountStatus())
    throw new Error("Önce hesabınla giriş yapmalısın.");
  if (!mirror || !observationPrepareKeys)
    throw new Error("Telefonun SQLite defteri henüz hazır değil.");
  // Keys arrive through the protected account relay. Do not assume the
  // phone has per-epoch read keys merely because it can sign in.
  await observationPrepareKeys();
  if (client !== account || !pilotAccountStatus())
    throw new Error("Hesap değişti; sunucu konumları alınmadı.");
  const result = await mirror.pullAvailable(account);
  observationsChanged?.();
  return result;
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
  observationsChanged?.();
  if (prior) await prior.signOut();
}
