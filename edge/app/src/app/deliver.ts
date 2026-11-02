// Cihaza bağlanınca sıradakileri sormadan işle (sahip kararı 2026-11-02).
// Önce ayarlar, sonra alanlar. Bir işlem ancak cihaz onaylayınca "işlendi",
// cihaz "hayır" derse "reddedildi" olur; yanıt gelmezse sırada kalır.
import type { LineConsole } from "./addTracker.ts";
import type { OpTarget, OpVia, FencePayload } from "./deviceOps.ts";
import type { SettingValues } from "./deviceSettings.ts";
import { DeviceRejected, writeFence, type FenceSummary } from "./fenceUsb.ts";
import { writeWaitingUsb } from "./trackerSettings.ts";
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

export async function deliverUsb(con: LineConsole, d: DeliverDeps): Promise<Delivered> {
  const out: Delivered = { wrote: [], problems: [] };
  // Ayarlar: istenen ≠ cihazdaki ise yazılır; sıradaki ayar işlemi sonuçlanır.
  try {
    const done = await writeWaitingUsb(con, d.wanted, d.has);
    if (Object.keys(done).length) { await d.reportSettings(done); out.wrote.push("ayarlar"); }
    if (d.openSettings) await d.result(d.openSettings.op, Object.keys(done).length ? "applied" : "same");
  } catch (e) {
    if (e instanceof DeviceRejected && d.openSettings) await d.result(d.openSettings.op, "rejected", short(e));
    out.problems.push(short(e));
  }
  // Alanlar: yalnız "Tasmalara uygula" ile açılmış işlem gönderilir.
  if (d.openFence) {
    const p = d.openFence.payload as FencePayload;
    const zones: Zone[] = p.zones.map((z) => ({ id: z.id, name: z.name, ring: z.ring, active: true, updated: 0 }));
    try {
      const sum = await writeFence(con, zones);
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

export const VIA_USB: OpVia = "usb";
