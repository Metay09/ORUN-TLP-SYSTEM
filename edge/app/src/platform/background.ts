// Keeps the app alive while a Gateway link is up, or while the signed-in
// phone watches the herd through the server (screen off, app in the
// background): an Android foreground service with a silent, ongoing
// notification. Type connectedDevice with a Gateway, specialUse without. Without it Android may freeze or kill the
// app mid-transfer. Nothing is lost if that happens anyway: the Gateway frees
// only records the phone has confirmed.
import { ForegroundService, Importance } from "@capawesome-team/capacitor-android-foreground-service";
import { Capacitor } from "@capacitor/core";

const CHANNEL = "orun-link";
const NOTIFICATION_ID = 7001;
// android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
const SERVICE_TYPE_CONNECTED_DEVICE = 16;
// android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
const SERVICE_TYPE_SPECIAL_USE = 1 << 30;
const UPDATE_MIN_MS = 5000;
export type KeepMode = "link" | "watch";
const TITLE: Record<KeepMode, string> = { link: "ORUN Edge gateway'e bağlı", watch: "ORUN sürüyü izliyor" };

export class KeepAlive {
  private running = false;
  private channelReady = false;
  private shown = "";
  private wanted: string | null = null;   // null: no notification wanted; else "mode\nbody"
  private lastUpdate = 0;
  private busy = false;
  private failed = false;   // one error per link: no retry loop, no log flood
  private timer: ReturnType<typeof setTimeout> | null = null;
  private log: (line: string) => void;

  constructor(log: (line: string) => void) { this.log = log; }

  /** Show or refresh the ongoing notification (calls are coalesced). */
  on(body: string, mode: KeepMode = "link"): void {
    if (this.failed) return;
    this.wanted = `${mode}\n${body}`;
    void this.apply();
  }
  off(): void { this.failed = false; this.wanted = null; void this.apply(); }

  // One plugin call at a time, always towards the latest wanted state;
  // text refreshes at most every 5 s.
  private async apply(): Promise<void> {
    if (this.busy || Capacitor.getPlatform() !== "android") return;
    const want = this.wanted;
    if (want === null && !this.running) return;
    if (want !== null && this.running && want === this.shown) return;
    if (want !== null && this.running && Date.now() - this.lastUpdate < UPDATE_MIN_MS) {
      if (!this.timer) this.timer = setTimeout(() => { this.timer = null; void this.apply(); },
                                               UPDATE_MIN_MS);
      return;
    }
    this.busy = true;
    try {
      if (want === null) {
        this.running = false;
        this.shown = "";
        await ForegroundService.stopForegroundService();
      } else {
        if (!this.channelReady) {
          const perm = await ForegroundService.checkPermissions();
          if (perm.display !== "granted") await ForegroundService.requestPermissions();
          await ForegroundService.createNotificationChannel({
            id: CHANNEL, name: "Arka planda çalışma",
            description: "ORUN bir gateway'e bağlıyken ya da sürüyü izlerken görünür", importance: Importance.Low,
          });
          this.channelReady = true;
        }
        const mode: KeepMode = want.startsWith("watch\n") ? "watch" : "link";
        const options = {
          id: NOTIFICATION_ID, title: TITLE[mode], body: want.slice(want.indexOf("\n") + 1),
          smallIcon: "ic_stat_orun", silent: true, notificationChannelId: CHANNEL,
          serviceType: (mode === "watch" ? SERVICE_TYPE_SPECIAL_USE : SERVICE_TYPE_CONNECTED_DEVICE) as never,
        };
        if (this.running) await ForegroundService.updateForegroundService(options);
        else await ForegroundService.startForegroundService(options);
        this.running = true;
        this.shown = want;
        this.lastUpdate = Date.now();
      }
    } catch (e) {
      // The link still works in the foreground; only background running fails.
      this.failed = true;
      this.wanted = this.running ? this.shown : null;
      this.busy = false;
      this.log(`[arka plan] bildirim hatası: ${e instanceof Error ? e.message : String(e)}; uygulamayı önde tut`);
      return;
    }
    this.busy = false;
    if (this.wanted !== (this.running ? this.shown : null)) void this.apply();
  }
}
