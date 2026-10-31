// ORUN S6B pilot account runtime: holds account bearer in process memory only.
// Navigating between app screens does not create another auth session.
// No localStorage, preferences, SQLite, browser cache or URL credential writes.
// This runtime does not yet automatically sync herd, zones or device commands.
import {
  OwnerAccountClient, type AccountIdentity,
} from "./ownerAccountClient.ts";
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";
import { ORUN_ACCOUNT_ORIGIN } from "./orunServiceConfig.ts";
import {
  AccountSharedMirror, type SharedMirrorSnapshot, type MirrorPullResult,
} from "./accountSharedMirror.ts";

let client: OwnerAccountClient | null = null;
let busy = false;
let sqliteMirror: AccountSharedMirror | null = null;

/** Registered only by Controller after its existing EdgeCore SQLite is ready.
 * This is an opt-in hook; no table is created and no sync starts on boot.
 */
export function attachPilotAccountSqliteMirror(mirror: AccountSharedMirror): void {
  sqliteMirror = mirror;
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
  if (prior) await prior.signOut();
}
