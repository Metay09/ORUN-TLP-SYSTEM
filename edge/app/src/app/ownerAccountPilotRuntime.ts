import { EdgeKeyBook } from "./edgeKeys.ts";
import { edgeKeyVault } from "../platform/orunEdgeKeys.ts";
// ORUN S6B pilot account runtime: holds account bearer in process memory only.
// Navigating between app screens does not create another auth session.
// No localStorage, preferences, SQLite, browser cache or URL credential writes.
// Server positions sync automatically while signed in: newest position of
// every tracker first, then history from the saved cursor (1. adım).
import {
  OwnerAccountClient, type AccountIdentity, type FarmGateway, type EdgeQueueSnapshot,
} from "./ownerAccountClient.ts";
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";
import { ORUN_ACCOUNT_ORIGIN } from "./orunServiceConfig.ts";
import { DeviceNameBook } from "./deviceNames.ts";
import { batteryCapacity } from "./batteryLife.ts";
import type { LoraEvent, LoraServer } from "./loraCarrier.ts";
import { onBleRevision, type StoreRevision } from "./loraRevision.ts";
import { hybridSource, LocalLoraSource } from "./loraLocal.ts";
import { TrackerFenceBook, type FenceRecord } from "./trackerFences.ts";
import { DeviceSettingsBook, type SettingValues, type Via as SettingsVia } from "./deviceSettings.ts";
import { DeviceOpsBook, type FencePayload, type OpVia } from "./deviceOps.ts";
import { WriteKeyBook } from "./writeKeys.ts";
import { forgetAccount, loadAccount, storeAccount } from "../platform/orunAccountSession.ts";
import { AccountObservationMirror } from "./accountObservationMirror.ts";
import type { Point, RecordId } from "../core/points.ts";
import {
  AccountSharedMirror, type SharedMirrorSnapshot, type MirrorPullResult,
} from "./accountSharedMirror.ts";

let client: OwnerAccountClient | null = null;
let busy = false;
let sqliteMirror: AccountSharedMirror | null = null;
let observationMirror: AccountObservationMirror | null = null;
let observationsChanged: (() => void) | null = null;
let observationPrepareKeys: (() => Promise<void>) | null = null;

// 1b: account state listeners (login, restore, logout, e-mail).
const accountListeners = new Set<() => void>();
let accountEmail: string | null = null;
let restoring: Promise<void> | null = null;
function accountChanged(): void { for (const fn of accountListeners) fn(); }
export function onPilotAccountChange(fn: () => void): () => void {
  accountListeners.add(fn);
  return () => { accountListeners.delete(fn); };
}
export function pilotAccountEmail(): string | null { return accountEmail; }
// False until the app knows whether this phone is signed in. Until then an
// empty herd means "not read yet", not "no animals" (screens say Yükleniyor).
let accountSettled = false;
export function pilotAccountSettled(): boolean { return accountSettled; }

async function persist(account: OwnerAccountClient): Promise<void> {
  const stored = account.storableSession();
  if (stored) await storeAccount(ORUN_ACCOUNT_ORIGIN, stored.identity, stored.accessToken)
    .catch(() => { /* login still works for this run */ });
}

/** 1b: on app start, bring back the login kept on this phone. Works offline;
 * when the server is reachable it confirms the session (or signs out). */
export function pilotRestoreAccount(): Promise<void> {
  if (restoring) return restoring;
  restoring = (async () => {
    if (client) return;
    const stored = await loadAccount(ORUN_ACCOUNT_ORIGIN);
    if (!stored || client) return;
    const restored = new OwnerAccountClient(ORUN_ACCOUNT_ORIGIN);
    restored.adopt(stored.identity, stored.accessToken);
    client = restored;
    await edgeKeys.load().catch(() => undefined);
    observationsChanged?.();
    startAutoSync();
    accountSettled = true;
    accountChanged();
    const check = await restored.refresh();
    if (client !== restored) return;
    if (check.state === "expired") {
      client = null;
      stopAutoSync();
      writeKeys.clear();
      edgeKeys.clear();
      await forgetAccount();
      observationsChanged?.();
    } else if (check.state === "ok") {
      accountEmail = check.email;
      await persist(restored);
    }
    accountChanged();
  })().finally(() => {
    restoring = null;
    if (!accountSettled) { accountSettled = true; accountChanged(); }   // signed out, or the keystore failed
  });
  return restoring;
}

/** 1d/1e: the signed-in account, for adding a device. */
export function pilotDeviceSetupClient(): OwnerAccountClient {
  if (!client || !pilotAccountStatus()) throw new Error("Önce hesabınla giriş yapmalısın.");
  return client;
}

/** 2d: device names (account, with a copy on the phone). */
export const deviceNames = new DeviceNameBook(
  typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);

export function pilotSetDeviceName(device: string, name: string): Promise<void> {
  return deviceNames.set(device, name, client && pilotAccountStatus() ? client : null);
}

/** Y3: battery capacity per tracker, in the account (not on the tracker). */
/**
 * 6c/6d: LoRa commands while this phone bridges a gateway: the account signs
 * them online; without internet this phone signs them with the tasmas'
 * write keys it already holds (loraLocal.ts). Signed out: nothing.
 */
let loraLocal: LocalLoraSource | null = null;
export function pilotLoraServer(): LoraServer | null {
  if (!writeKeys.size && !(client && pilotAccountStatus())) return null;
  loraLocal ??= new LocalLoraSource({
    ops: () => deviceOps.snapshot(),
    key: (d) => writeKeys.active(d),
    reported: (d) => (deviceSettings.get(d)?.reported ?? null) as Record<string, unknown> | null,
    opResult: (op, device, state, note) => deviceOps.result(op, device, state, "lora", settingsServer(), note),
    settingsApplied: (device, values) => deviceSettings.report(device, values, "lora", settingsServer()),
    areasApplied: (device, payload, r) => trackerFences.set(device, {
      digest: payload.digest, areas: r.areas, vertices: r.vertices,
      zones: (payload.zones ?? []).map((z) => ({ id: z.id, name: z.name })), appliedAt: Date.now(),
    }, client && pilotAccountStatus() ? client : null),
    random32: () => crypto.getRandomValues(new Uint32Array(1))[0],
  }, typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);
  return hybridSource(client && pilotAccountStatus() ? client : null, loraLocal);
}

/**
 * A USB/Bluetooth write moved a tasma's settings or areas to a new revision:
 * the account (and this phone's offline signer) sign the next LoRa command
 * for it, so it goes in one report without a probe. Kept until sent.
 */
let revisionsPending: LoraEvent[] = [];
export function pilotNoteRevision(device: string, r: StoreRevision): void {
  const d = device.toUpperCase();
  pilotLoraServer();   // makes the local signer if this phone can sign
  loraLocal?.noteRevision(d, r.resource, r.revision);
  revisionsPending = [
    ...revisionsPending.filter((e) => !(e.kind === "revision" && e.device === d && e.resource === r.resource)),
    { kind: "revision" as const, device: d, resource: r.resource, revision: r.revision },
  ].slice(-32);
  void flushRevisions();
}
async function flushRevisions(): Promise<void> {
  if (!revisionsPending.length || !client || !pilotAccountStatus()) return;
  const batch = revisionsPending;
  try {
    await client.loraEvents(batch);
    revisionsPending = revisionsPending.filter((e) => !batch.includes(e));
  } catch { /* offline: sent with the next one or the next refresh */ }
}
onBleRevision(pilotNoteRevision);

export function pilotSetBatteryCapacity(device: string, mAh: number | null): Promise<void> {
  return batteryCapacity.set(device, mAh, client && pilotAccountStatus() ? client : null);
}

/** 3a: which zones each tracker holds. */
export const trackerFences = new TrackerFenceBook(
  typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);

export function pilotRecordFence(device: string, rec: FenceRecord): Promise<void> {
  return trackerFences.set(device, rec, client && pilotAccountStatus() ? client : null);
}

/** 4b: device settings (account, with a copy on the phone). */
export const deviceSettings = new DeviceSettingsBook(
  typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);

const settingsServer = () => (client && pilotAccountStatus() ? client : null);

/** 4c-1b: Bluetooth yazma anahtarları (hesaptan; çıkınca silinir). */
export const edgeKeys = new EdgeKeyBook(edgeKeyVault);
export const writeKeys = new WriteKeyBook(
  typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);

/** İşlemler: alan ve ayar gönderme kaydı (hesapta, telefonda bir kopya). */
export const deviceOps = new DeviceOpsBook(
  typeof localStorage !== "undefined" ? (() => { try { return localStorage; } catch { return null; } })() : null);

/** Ayar kaydı = istenen değer + bir işlem (cihaz onaylayınca "işlendi"). */
export async function pilotWantSettings(devices: string[], set: SettingValues): Promise<void> {
  await deviceSettings.want(devices, set, settingsServer());
  await deviceOps.create("settings", devices.map((d) => ({ device: d, payload: { set: set as Record<string, number | boolean> } })), settingsServer());
}

export function pilotCreateFenceOp(items: { device: string; payload: FencePayload }[]): Promise<string> {
  return deviceOps.create("fence", items, settingsServer());
}

export function pilotOpResult(op: string, device: string, state: "applied" | "same" | "rejected", via: OpVia, note?: string): Promise<void> {
  return deviceOps.result(op, device, state, via, settingsServer(), note);
}

export function pilotReportSettings(device: string, values: SettingValues, via: SettingsVia): Promise<void> {
  return deviceSettings.report(device, values, via, settingsServer());
}

function refreshNames(): void {
  if (client && pilotAccountStatus()) {
    void deviceNames.refresh(client).catch(() => undefined);
    void batteryCapacity.refresh(client).catch(() => undefined);
    void trackerFences.refresh(client).catch(() => undefined);
    void deviceSettings.refresh(client).catch(() => undefined);
    void deviceOps.refresh(client).catch(() => undefined);
    void writeKeys.refresh(client).catch(() => undefined);
    void edgeKeys.refresh(client).catch(() => undefined);
    void flushRevisions();
  }
}

/** 2c: the farm's gateways, or null when signed out. */
export async function pilotGateways(): Promise<FarmGateway[] | null> {
  if (!client || !pilotAccountStatus()) return null;
  return client.gateways();
}

/** Read-only, account-authorized PC spool status (not a direct Gateway ACK). */
export async function pilotEdgeQueues(): Promise<EdgeQueueSnapshot | null> {
  if (!client || !pilotAccountStatus()) return null;
  return client.edgeQueues();
}

let gatewayReport = { key: "", at: 0 };
/** 2c: while this phone bridges a gateway, tell the server (at most every 2 min). */
export function pilotReportGateway(gateway: string, transport: "ble" | "usb",
                                   held: number | null, capacity: number | null): void {
  if (!client || !pilotAccountStatus() || !/^[0-9A-Fa-f]{16}$/.test(gateway)) return;
  const key = `${gateway}|${transport}`;
  const now = Date.now();
  if (key === gatewayReport.key && now - gatewayReport.at < 120_000) return;
  gatewayReport = { key, at: now };
  void client.reportGateway({ gateway: gateway.toUpperCase(), transport, held, capacity }).catch(() => {
    gatewayReport = { key: "", at: 0 };     // try again on the next change
  });
}

/** 2: the signed-in account, for offline map packs; null when signed out. */
export function pilotMapClient(): OwnerAccountClient | null {
  return client && pilotAccountStatus() ? client : null;
}

/** Optional login e-mail. */
export async function pilotSetEmail(email: string): Promise<string> {
  if (!client || !pilotAccountStatus()) throw new Error("Önce hesabınla giriş yapmalısın.");
  accountEmail = await client.setEmail(email);
  accountChanged();
  return accountEmail;
}

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
  void pilotRestoreAccount().catch(() => { /* stays signed out */ });
}

/** Only active account's verified remote records; logout reveals none. */
export async function pilotAccountObservationPoints(unlocated?: RecordId[]): Promise<Point[]> {
  if (!client || !pilotAccountStatus() || !observationMirror) return [];
  return observationMirror.points(client, unlocated);
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
    last => { noteBackgroundSync(last.at); setSync({ running: false, last, error: null }); },
    e => setSync({ running: false, error: e instanceof Error ? e.message : String(e) }),
  ).finally(() => { if (syncRun === run) syncRun = null; });
  return run;
}

/** Kept for callers of the old manual button. */
export const pilotAccountDownloadObservations = pilotAccountSync;

// Evidence, not a promise: when the last sync finished while the app was NOT
// on screen. The connection screen shows it, so "does it really sync in the
// background on this phone?" is answered by looking (owner, 2026-11-14).
const BG_KEY = "orun.app.bgsync.v1";
function noteBackgroundSync(at: number): void {
  if (typeof document === "undefined" || document.visibilityState !== "hidden") return;
  try { localStorage.setItem(BG_KEY, String(at)); } catch { /* evidence only */ }
}
export function pilotBackgroundSyncAt(): number | null {
  try { const v = Number(localStorage.getItem(BG_KEY)); return Number.isFinite(v) && v > 0 ? v : null; } catch { return null; }
}

// Set while the Android foreground service keeps the app running: the
// minute-by-minute pull then continues with the screen off.
let backgroundWatch = false;
export function setPilotBackgroundWatch(on: boolean): void { backgroundWatch = on; }

function backgroundSync(): void {
  if (!client || !pilotAccountStatus() || !observationMirror) return;
  if (!backgroundWatch && typeof document !== "undefined" && document.visibilityState === "hidden") return;
  void pilotAccountSync().catch(() => { /* shown via pilotObservationSyncState */ });
}

function startAutoSync(): void {
  stopAutoSync();
  backgroundSync();
  refreshNames();
  autoTimer = setInterval(() => { backgroundSync(); refreshNames(); }, AUTO_SYNC_MS);
  if (typeof document !== "undefined") {
    onVisible = () => { if (document.visibilityState === "visible") { backgroundSync(); refreshNames(); } };
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
    // Drop previous account's signing authority before any network request.
    // This also invalidates in-flight key refreshes through WriteKeyBook.clear().
    writeKeys.clear();
    edgeKeys.clear();
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
    await persist(next);
    accountEmail = (await next.refresh()).email;
    accountChanged();
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
    // Drop previous account's signing authority before any network request.
    // This also invalidates in-flight key refreshes through WriteKeyBook.clear().
    writeKeys.clear();
    edgeKeys.clear();
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
  accountEmail = null;
  stopAutoSync();
  setSync({ running: false, last: null, error: null });
  writeKeys.clear();
      edgeKeys.clear();
  await forgetAccount();
  observationsChanged?.();
  accountChanged();
  if (prior) await prior.signOut();
}
