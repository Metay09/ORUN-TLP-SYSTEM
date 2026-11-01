// 4b: tasma ayarlarını USB ile değiştir (rapor aralığı, hizmetler). Tasma
// ayarı kalıcı belleğe yazar ve "APP SET" satırıyla sonucu söyler; ayar ancak
// bu satır APPLIED/UNCHANGED derse yazılmış sayılır.
import type { LineConsole } from "./addTracker.ts";

/** Tasmanın kabul ettiği sınırlar (firmware config_mutation.h, gnss_config.h). */
export const INTERVAL = { minS: 60, maxS: 12 * 24 * 3600 } as const;

/** Seçilebilir aralıklar (saniye). */
export const INTERVAL_CHOICES = [60, 120, 180, 300, 600, 900, 1800, 3600, 7200, 14400, 21600, 43200, 86400];

export interface ServiceChoice { tracking: boolean; relay: boolean; }

/** "APP SERVICES T" / "TR" / "R" / "NONE": tasma için alıcı (A) hiç gönderilmez. */
export function servicesCommand(s: ServiceChoice): string {
  const letters = (s.tracking ? "T" : "") + (s.relay ? "R" : "");
  return `APP SERVICES ${letters || "NONE"}`;
}

export function intervalCommand(seconds: number): string {
  if (!Number.isInteger(seconds) || seconds < INTERVAL.minS || seconds > INTERVAL.maxS)
    throw new Error("Rapor aralığı 1 dakika ile 12 gün arasında olmalı.");
  return `APP INTERVAL ${seconds}`;
}

const SET = /^APP SET id=\d+ code=(\w+)/;
const BUSY = /^(APP BUSY|APP REJECTED)/;
const WHY: Record<string, string> = {
  INVALID: "tasma bu değeri kabul etmedi",
  ACCESS_DENIED: "izin yok",
  MAINTENANCE: "tasmanın ayar belleği bakımda",
  UNAVAILABLE: "tasmanın ayar belleği hazır değil",
  OUTCOME_UNKNOWN: "yazılıp yazılmadığı belli değil; durumu yeniden oku",
};

/** Send one setting line; resolves "applied" or "unchanged", throws otherwise. */
export async function applySetting(con: LineConsole, line: string): Promise<"applied" | "unchanged"> {
  await con.send(line);
  const r = await con.waitFor((l) => (SET.test(l) || BUSY.test(l) ? l : null), 15_000);
  if (!r) throw new Error("Tasmadan yanıt gelmedi; ayarın yazıldığını varsayma, durumu yeniden oku.");
  if (BUSY.test(r)) throw new Error("Tasma şu an meşgul; birkaç saniye sonra tekrar dene.");
  const code = SET.exec(r)![1];
  if (code === "APPLIED") return "applied";
  if (code === "UNCHANGED") return "unchanged";
  throw new Error(`Ayar yazılamadı: ${WHY[code] ?? code}.`);
}

/**
 * 4b: write what is waiting (wanted ≠ on the device) over USB. `has` is what
 * the tracker reported just now. Returns the values the tracker accepted.
 */
export async function writeWaitingUsb(
  con: LineConsole, wanted: { interval_s?: number; tracking?: boolean; relay?: boolean },
  has: { interval_s?: number; tracking?: boolean; relay?: boolean },
): Promise<{ interval_s?: number; tracking?: boolean; relay?: boolean }> {
  const done: { interval_s?: number; tracking?: boolean; relay?: boolean } = {};
  if (wanted.interval_s !== undefined && wanted.interval_s !== has.interval_s) {
    await applySetting(con, intervalCommand(wanted.interval_s));
    done.interval_s = wanted.interval_s;
  }
  const svcWanted = (wanted.tracking !== undefined && wanted.tracking !== has.tracking) ||
                    (wanted.relay !== undefined && wanted.relay !== has.relay);
  if (svcWanted) {
    const s = { tracking: wanted.tracking ?? has.tracking ?? true, relay: wanted.relay ?? has.relay ?? false };
    await applySetting(con, servicesCommand(s));
    done.tracking = s.tracking; done.relay = s.relay;
  }
  return done;
}
