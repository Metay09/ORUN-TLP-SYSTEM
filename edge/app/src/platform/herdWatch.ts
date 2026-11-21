// The app's handle on the native herd watch (android HerdWatchService.java):
// the small service that asks the account server for herd alerts and shows
// them, also with the app closed. On the web/PC build all of this is a no-op.
import { Capacitor, registerPlugin } from "@capacitor/core";

interface Native {
  localGateway(input: { site: string; actor: string; usb: boolean; measured: string }): Promise<void>;
  start(): Promise<void>;
  stop(): Promise<void>;
  status(): Promise<{ lastOk?: number; line?: string; latest?: string }>;
  historySeed(input: HistoryIdentity & { cursor: number }): Promise<void>;
  historyAck(input: HistoryIdentity & { cursor: number }): Promise<void>;
  historyPending(): Promise<{ stage?: string | null }>;
}
const native = registerPlugin<Native>("OrunHerdWatch");
const android = () => Capacitor.getPlatform() === "android";

export function shareLocalGateway(site: string, actor: string, usb: boolean, measured: string): void {
  if (android()) void native.localGateway({ site, actor, usb, measured }).catch(() => {});
}

export async function startHerdWatch(): Promise<boolean> {
  if (!android()) return false;
  try { await native.start(); return true; } catch { return false; }
}

export async function stopHerdWatch(): Promise<void> {
  if (!android()) return;
  try { await native.stop(); } catch { /* nothing was running */ }
}

/** The native cache contains UNVERIFIED ciphertext, never a SQLite receipt. */
export interface HistoryIdentity {
  site: string;
  actor: string;
  installation: string;
}
export interface StagedHistoryPage {
  after: number;
  body: unknown;
}

export async function seedNativeHistory(identity: HistoryIdentity, cursor: number): Promise<void> {
  if (!android()) return;
  await native.historySeed({ ...identity, cursor });
}

export async function ackNativeHistory(identity: HistoryIdentity, cursor: number): Promise<void> {
  if (!android()) return;
  await native.historyAck({ ...identity, cursor });
}

export async function pendingNativeHistory(): Promise<StagedHistoryPage[]> {
  if (!android()) return [];
  const { stage } = await native.historyPending();
  if (!stage) return [];
  const doc: unknown = JSON.parse(stage);
  if (!doc || typeof doc !== "object" || Array.isArray(doc)) return [];
  const pages = (doc as { pages?: unknown }).pages;
  if (!Array.isArray(pages) || pages.length > 32) return [];
  return pages.filter((p): p is StagedHistoryPage =>
    !!p && typeof p === "object" && !Array.isArray(p) &&
    typeof p.after === "number" && Number.isSafeInteger(p.after) &&
    p.after >= 0 && "body" in p);
}

export interface HerdWatchStatus {
  /** When the service last reached the server (ms), or null. */
  lastOk: number | null;
  /** The newest-positions reply it kept: { at, actor, body } as JSON text, or null. */
  latest: string | null;
}

export async function herdWatchStatus(): Promise<HerdWatchStatus> {
  if (!android()) return { lastOk: null, latest: null };
  try {
    const s = await native.status();
    return { lastOk: typeof s.lastOk === "number" && s.lastOk > 0 ? s.lastOk : null,
             latest: typeof s.latest === "string" ? s.latest : null };
  } catch { return { lastOk: null, latest: null }; }
}
