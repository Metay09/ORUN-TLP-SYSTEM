// 4b: tasma ayarlarını USB ile değiştir (rapor aralığı, hizmetler). Tasma
// ayarı kalıcı belleğe yazar ve "APP SET" satırıyla sonucu söyler; ayar ancak
// bu satır APPLIED/UNCHANGED derse yazılmış sayılır.
import type { LineConsole } from "./addTracker.ts";
import { DeviceRejected } from "./fenceUsb.ts";

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

/** 4c-1a: rapordan sonra Bluetooth (saniye, 0 = kapalı); tasmanın sınırı 300. */
export const BLE_WINDOW = { maxS: 300 } as const;
export const BLE_WINDOW_CHOICES = [0, 15, 30, 60, 120, 300];
export function bleWindowCommand(seconds: number): string {
  if (!Number.isInteger(seconds) || seconds < 0 || seconds > BLE_WINDOW.maxS)
    throw new Error("Bluetooth süresi 0 ile 300 saniye arasında olmalı.");
  return `APP BLE WINDOW ${seconds}`;
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
  if (code === "INVALID" || code === "ACCESS_DENIED") throw new DeviceRejected(`Tasma reddetti: ${WHY[code]}.`);
  throw new Error(`Ayar yazılamadı: ${WHY[code] ?? code}.`);
}

/**
 * 4b: write what is waiting (wanted ≠ on the device) over USB. `has` is what
 * the tracker reported just now. Returns the values the tracker accepted.
 */
type Values = { interval_s?: number; tracking?: boolean; relay?: boolean; ble_window_s?: number };

export async function writeWaitingUsb(con: LineConsole, wanted: Values, has: Values): Promise<Values> {
  const done: Values = {};
  if (wanted.interval_s !== undefined && wanted.interval_s !== has.interval_s) {
    await applySetting(con, intervalCommand(wanted.interval_s));
    done.interval_s = wanted.interval_s;
  }
  if (wanted.ble_window_s !== undefined && wanted.ble_window_s !== has.ble_window_s) {
    await applySetting(con, bleWindowCommand(wanted.ble_window_s));
    done.ble_window_s = wanted.ble_window_s;
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
