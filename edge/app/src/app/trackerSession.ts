// Tasmaya açık Bluetooth bağlantısı (sahip isteği 2026-11-11): pencere ya da
// ekran kapanınca bağlantı kopmaz; kullanıcı "Bağlantıyı kes" deyene ya da
// tasma uzaklaşana kadar açık kalır. Aynı anda tek tasma; yenisi açılınca
// eskisi kapanır. Bağlantı sekmesi ve cihaz ekranı buradan "bağlı" görür.
import type { TrackerStatus } from "./trackerStatus.ts";
import type { TrackerBle } from "../platform/trackerBle.ts";

/** What the session needs from a link (TrackerBle on the phone, a fake in tests). */
type Link = Pick<TrackerBle, "alive" | "close" | "exchange" | "readRaw">;
type Connect = (bleId: string, onGone: () => void) => Promise<TrackerBle>;

export interface TrackerSession {
  bleId: string;
  device: string | null;      // 16 hex, upper case; null until the device says who it is
  since: number;
  status: TrackerStatus | null;
  link: TrackerBle;
}

let cur: TrackerSession | null = null;
const fns = new Set<() => void>();
const emit = () => { for (const f of fns) f(); };

export const trackerSession = {
  subscribe(fn: () => void): () => void { fns.add(fn); return () => { fns.delete(fn); }; },
  snapshot(): TrackerSession | null { return cur; },

  /** The live session for this tracker ID (or Bluetooth device), if any. */
  find(device: string | null, bleId?: string | null): TrackerSession | null {
    if (!cur || !cur.link.alive) return null;
    if (bleId && cur.bleId === bleId) return cur;
    if (device && cur.device && cur.device === device.toUpperCase()) return cur;
    return null;
  },

  /** The open link to this Bluetooth device, or a new one (closing any other). */
  async open(bleId: string, device: string | null, connect: Connect): Promise<TrackerBle> {
    const same = this.find(device, bleId);
    if (same) {
      if (device && !same.device) { cur = { ...same, device: device.toUpperCase() }; emit(); }
      return same.link;
    }
    await this.close();
    const holder: { link: Link | null } = { link: null };
    const link = await connect(bleId, () => {
      if (cur && cur.link === holder.link) { cur = null; emit(); }
    });
    holder.link = link;
    cur = { bleId, device: device ? device.toUpperCase() : null, since: Date.now(), status: null, link };
    emit();
    return link;
  },

  /** Latest reading over this link (and who the device turned out to be). */
  note(link: TrackerBle, status: TrackerStatus, device: string | null): void {
    if (!cur || cur.link !== link) return;
    cur = { ...cur, status, device: device ? device.toUpperCase() : cur.device };
    emit();
  },

  async close(): Promise<void> {
    const c = cur;
    if (!c) return;
    cur = null;
    emit();
    await c.link.close();
  },
};
