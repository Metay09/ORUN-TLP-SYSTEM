// Cihaza bağlanınca sıradakileri sormadan işle (sahip kararı 2026-11-02).
// Önce ayarlar, sonra alanlar; USB ve Bluetooth'tan aynı (4c-1b, 4c-2). Bir işlem ancak cihaz onaylayınca "işlendi",
// cihaz "hayır" derse "reddedildi" olur; yanıt gelmezse sırada kalır.
import type { LineConsole } from "./addTracker.ts";
import type { OpTarget, OpVia, FencePayload } from "./deviceOps.ts";
import type { SettingValues } from "./deviceSettings.ts";
import { bleSettingWriter, bleWriteFence, type Exchange, type KeyFor } from "./bleWrite.ts";
import { DeviceRejected, writeFence, type FenceSummary } from "./fenceUsb.ts";
import { usbWriter, writeWaiting, type SettingWriter } from "./trackerSettings.ts";
import type { Zone } from "./zones.ts";

export interface DeliverDeps {
  wanted: SettingValues;                       // istenen ayarlar
  has: SettingValues;                          // cihazın az önce söylediği
  openSettings: OpTarget | null;
  openFence: OpTarget | null;
  reportSettings(values: SettingValues): Promise<void>;
  recordFence(sum: FenceSummary, zones: { id: string; name: string }[]): Promise<void>;
  result(op: string, state: "applied" | "same" | "rejected", note?: string): Promise<void>;
}

export interface Delivered { wrote: string[]; problems: string[]; }

const short = (e: unknown) => (e instanceof Error ? e.message : String(e)).slice(0, 120);

/** Ayarlar: istenen ≠ cihazdaki ise yazılır; sıradaki ayar işlemi sonuçlanır (USB ya da Bluetooth). */
export async function deliverSettings(w: SettingWriter, d: Omit<DeliverDeps, "openFence" | "recordFence">): Promise<Delivered> {
  const out: Delivered = { wrote: [], problems: [] };
  try {
    const done = await writeWaiting(w, d.wanted, d.has);
    if (Object.keys(done).length) { await d.reportSettings(done); out.wrote.push("ayarlar"); }
    if (d.openSettings) await d.result(d.openSettings.op, Object.keys(done).length ? "applied" : "same");
  } catch (e) {
    if (e instanceof DeviceRejected && d.openSettings) await d.result(d.openSettings.op, "rejected", short(e));
    out.problems.push(short(e));
  }
  return out;
}

/** Önce ayarlar, sonra "Tasmalara uygula" ile açılmış alan işlemi; yol ne olursa olsun aynı sıra. */
export async function deliver(w: SettingWriter, writeZones: (zones: Zone[]) => Promise<FenceSummary>, d: DeliverDeps): Promise<Delivered> {
  const out = await deliverSettings(w, d);
  if (d.openFence) {
    const p = d.openFence.payload as FencePayload;
    const zones: Zone[] = p.zones.map((z) => ({ id: z.id, name: z.name, ring: z.ring, active: true, updated: 0 }));
    try {
      const sum = await writeZones(zones);
      await d.recordFence(sum, p.zones.map((z) => ({ id: z.id, name: z.name })));
      await d.result(d.openFence.op, "applied");
      out.wrote.push(`alanlar (${sum.areas})`);
    } catch (e) {
      if (e instanceof DeviceRejected) await d.result(d.openFence.op, "rejected", short(e));
      out.problems.push(short(e));
    }
  }
  return out;
}

export function deliverUsb(con: LineConsole, d: DeliverDeps): Promise<Delivered> {
  return deliver(usbWriter(con), (zones) => writeFence(con, zones), d);
}

/** 4c-2: aynısı Bluetooth'tan (hesaptaki telefon, tasmanın imzalı onayı). */
export function deliverBle(exchange: Exchange, keyFor: KeyFor, device: string, d: DeliverDeps): Promise<Delivered> {
  return deliver(bleSettingWriter(exchange, keyFor, device), (zones) => bleWriteFence(exchange, keyFor, device, zones), d);
}

export const VIA_USB: OpVia = "usb";
