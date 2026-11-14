// Posts the herd notifications decided in app/alerts.ts (Android only; on the
// web/PC build this does nothing). Sound + heads-up: own channel, separate
// from the silent "gateway bağlı / sürü izleniyor" service notification.
import { Capacitor } from "@capacitor/core";
import { LocalNotifications } from "@capacitor/local-notifications";
import type { Alert } from "../app/alerts.ts";

const CHANNEL = "orun-alerts";
let ready: Promise<boolean> | null = null;

function prepare(): Promise<boolean> {
  ready ??= (async () => {
    let perm = await LocalNotifications.checkPermissions();
    if (perm.display !== "granted") perm = await LocalNotifications.requestPermissions();
    if (perm.display !== "granted") return false;
    await LocalNotifications.createChannel({
      id: CHANNEL, name: "Sürü uyarıları",
      description: "Alandan çıkan, dönen, haber alınamayan hayvan ve biten pil",
      importance: 4, visibility: 1,
    });
    return true;
  })().catch(() => false);
  return ready;
}

// Stable positive 31-bit id per alert key (Android notification ids are ints).
function idOf(key: string): number {
  let h = 2166136261;
  for (let i = 0; i < key.length; i++) h = Math.imul(h ^ key.charCodeAt(i), 16777619);
  return (h >>> 1) || 1;
}

/** Ask for the notification permission (the native herd watch needs it too). */
export async function ensureNotifyPermission(): Promise<boolean> {
  if (Capacitor.getPlatform() !== "android") return false;
  const ok = await prepare();
  if (!ok) ready = null;
  return ok;
}

/** True when the alerts were handed to Android (or there is nothing to do here). */
export async function postAlerts(alerts: Alert[]): Promise<boolean> {
  if (!alerts.length || Capacitor.getPlatform() !== "android") return true;
  if (!(await prepare())) { ready = null; return false; }   // ask again next time
  await LocalNotifications.schedule({
    notifications: alerts.map((a) => ({
      id: idOf(a.key), title: a.title, body: a.body, channelId: CHANNEL,
      smallIcon: "ic_stat_orun", extra: { asset: a.asset },
    })),
  });
  return true;
}
