// Telefon bildirimleri (5. adım, sahip seçimi 2026-11-13): alandan çıktı,
// alana döndü, haber alınamıyor / kayıp olabilir, pil bitiyor.
//
// Karar telefondaki olay listesinden verilir (events.ts deriveEvents); kayıt
// sunucudan da gelse, bağlı gateway'den de gelse aynı bildirim çıkar. Burada
// yalnız "hangisi daha önce bildirilmedi" hesabı var: yeni olay ÜRETMEZ.
//  - Her olay bir kez bildirilir (olay kimliği saklanır).
//  - İlk çalıştırmada eldekiler bildirilmez: uygulamayı yeni kuran kişi eski
//    olaylarla bildirim yağmuruna tutulmaz.
//  - Süren olay ne zaman başlamış olursa olsun bildirilir (telefon kapalıyken
//    çıkmış hayvan hâlâ dışarıdaysa haber verilir). "Alana döndü" yalnız
//    çıkışı bildirilmiş olaya ve son 12 saatte döndüyse söylenir.
//  - Telefonun elindeki veri bayatken hiç karar verilmez (watchFresh): yoksa
//    uzun süre kapalı kalmış telefon açılır açılmaz herkesi "sessiz" sanar.
import { fmt } from "./i18n/tr.ts";
import type { AppEvent } from "./model.ts";

export interface Alert { key: string; title: string; body: string; asset: string | null; }
export interface AlertMemory { ready: boolean; keys: string[]; }
export const EMPTY_MEMORY: AlertMemory = { ready: false, keys: [] };

const RECENT_MS = 12 * 3600_000;
const KEEP = 400;   // remembered keys (oldest dropped)
const NOTIFY = new Set(["out", "silent", "lost", "battery-critical"]);

/** Collar heard but without a position: not one of the owner's four notifications. */
const positionless = (e: AppEvent) => e.id.startsWith("nofix:") || e.id.startsWith("gpsfault:");

function candidates(events: AppEvent[], now: number): Alert[] {
  const out: Alert[] = [];
  for (const e of events) {
    if (!e.alert || !NOTIFY.has(e.kind) || positionless(e)) continue;
    if (e.end === null) {
      out.push({ key: e.id, title: e.title, body: e.detail, asset: e.asset });
    } else if (e.kind === "out" && now - e.end <= RECENT_MS) {
      // Came back: told only if the leaving itself was told (see nextAlerts).
      out.push({ key: `back:${e.id}`, title: e.title.replace(/ alan dışına çıktı$/, " alana döndü"),
                 body: `${fmt.duration(e.end - e.at)} dışarıda kaldı`, asset: e.asset });
    }
  }
  return out;
}

/** Alerts to show now, and the memory to store once they were handed over. */
export function nextAlerts(events: AppEvent[], memory: AlertMemory, now: number):
    { alerts: Alert[]; memory: AlertMemory } {
  const all = candidates(events, now);
  const known = new Set(memory.keys);
  if (!memory.ready) {
    // First run: remember what is already there, tell nothing.
    return { alerts: [], memory: { ready: true, keys: all.map((a) => a.key).slice(-KEEP) } };
  }
  const fresh = all.filter((a) => !known.has(a.key) && (!a.key.startsWith("back:") || known.has(a.key.slice(5))));
  if (all.every((a) => known.has(a.key))) return { alerts: [], memory };   // nothing new: same object
  const keys = [...memory.keys, ...all.map((a) => a.key).filter((k) => !known.has(k))].slice(-KEEP);
  return { alerts: fresh, memory: { ready: true, keys } };
}

/** Is what the phone holds current enough to decide on? Either the server
 * was read to the end a moment ago, or a gateway is attached right now. */
export const FRESH_MS = 5 * 60_000;
export function watchFresh(sync: { at: number; more: boolean } | null, linked: boolean, now: number): boolean {
  return linked || (sync !== null && !sync.more && now - sync.at <= FRESH_MS);
}
