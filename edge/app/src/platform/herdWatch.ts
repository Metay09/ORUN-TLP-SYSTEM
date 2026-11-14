// The app's handle on the native herd watch (android HerdWatchService.java):
// the small service that asks the account server for herd alerts and shows
// them, also with the app closed. On the web/PC build all of this is a no-op.
import { Capacitor, registerPlugin } from "@capacitor/core";

interface Native {
  start(): Promise<void>;
  stop(): Promise<void>;
  status(): Promise<{ lastOk?: number; line?: string; latest?: string }>;
}
const native = registerPlugin<Native>("OrunHerdWatch");
const android = () => Capacitor.getPlatform() === "android";

export async function startHerdWatch(): Promise<boolean> {
  if (!android()) return false;
  try { await native.start(); return true; } catch { return false; }
}

export async function stopHerdWatch(): Promise<void> {
  if (!android()) return;
  try { await native.stop(); } catch { /* nothing was running */ }
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
