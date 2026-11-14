// Read-only view: last-known counts keep their age and never become live claims.
// Tracker history count is NOT the number awaiting LoRa or a verified ACK.
import { fmt } from "../../app/i18n/tr.ts";
import type { FarmGateway, EdgeQueue } from "../../app/ownerAccountClient.ts";
import type { CollarStorageReading } from "../../app/collarStorageReadings.ts";

export interface TransferEntry {
  id: string; title: string; amount: string; detail: string; stale: boolean;
}
export interface TransferStage {
  id: "tracker" | "gateway" | "pc" | "server";
  title: string; entries: TransferEntry[];
}
interface PhoneGateway {
  id: string | null; connected: boolean; held: number | null; capacity: number | null;
  readAt: number | null;
}
export interface TransferInput {
  now: number;
  trackerIds: string[];
  collars: readonly CollarStorageReading[];
  gateways: readonly FarmGateway[] | null;
  localGateway: PhoneGateway;
  edges: readonly EdgeQueue[] | null;
  serverConfirmed: number | null;
  serverReadAt: number | null;
  label: (id: string) => string;
}
const FRESH_MS = 5 * 60_000;
const old = (at: number | null, now: number) => at === null || at > now || now - at > FRESH_MS;
const ago = (at: number | null, now: number, kind: string) => at === null
  ? kind + " zamanı bilinmiyor"
  : at > now ? kind + " zamanı geçersiz"
  : kind + " " + fmt.ago(at, now) + (old(at, now) ? " · eski bilgi" : "");
const ratio = (n: number | null, cap: number | null) =>
  (n === null ? "—" : fmt.num(n)) + " / " + (cap === null ? "—" : fmt.num(cap));
const empty = (id: string, title: string, detail: string): TransferEntry =>
  ({ id, title, amount: "—", detail, stale: true });

export function transferStages(i: TransferInput): TransferStage[] {
  const ids = new Set(i.trackerIds.map((x) => x.toUpperCase()));
  const samples = i.collars.filter((x) => ids.has(x.device.toUpperCase()))
    .sort((a, b) => a.device.localeCompare(b.device));
  const trackers: TransferEntry[] = samples.length ? samples.map((x) => ({
    id: x.device, title: i.label(x.device), amount: ratio(x.count, x.capacity),
    detail: "Bellekteki geçmiş kayıtları · " + x.via.toUpperCase() + " · " + ago(x.at, i.now, "Son okuma"),
    stale: old(x.at, i.now),
  })) : [empty("tracker-none", "Tasma belleği",
    "USB/Bluetooth bellek okuması yok. Geçmiş kayıt sayısı, LoRa bekleyen sayısı değildir.")];

  const gws = new Map<string, FarmGateway>();
  for (const g of i.gateways ?? []) gws.set(g.gateway.toUpperCase(), g);
  const local = i.localGateway;
  if (local.connected && local.id) {
    const id = local.id.toUpperCase();
    const prev = gws.get(id);
    const freshRead = local.held !== null && local.capacity !== null;
    gws.set(id, {
      gateway: id, via: prev?.via ?? null, held: local.held ?? prev?.held ?? null,
      capacity: local.capacity ?? prev?.capacity ?? null,
      statusAt: freshRead && local.readAt !== null ? local.readAt : prev?.statusAt ?? null,
      lastSeen: i.now, lastUpload: prev?.lastUpload ?? null,
      trackers24h: prev?.trackers24h ?? 0,
    });
  }
  const gateways: TransferEntry[] = [...gws.values()].sort((a,b) => a.gateway.localeCompare(b.gateway))
    .map((x) => ({
      id: x.gateway, title: i.label(x.gateway), amount: ratio(x.held, x.capacity),
      detail: "Gateway'de tutulan kayıt · " + ago(x.statusAt, i.now, "Son ölçüm"),
      stale: old(x.statusAt, i.now),
    }));
  if (!gateways.length) gateways.push(empty("gateway-none", "Gateway belleği", "Gateway sayacı henüz alınmadı."));

  const pc: TransferEntry[] = (i.edges ?? []).map((x) => ({
    id: x.edge, title: x.edge, amount: fmt.num(x.pending) + " bekliyor",
    detail: fmt.num(x.delivered) + " teslim edilmiş · " + fmt.num(x.quarantined) + " sorunlu · " + ago(x.lastSeen, i.now, "Son rapor"),
    stale: old(x.lastSeen, i.now),
  })).sort((a,b) => a.title.localeCompare(b.title));
  if (!pc.length) pc.push(empty("pc-none", "PC Edge", i.edges === null
    ? "PC Edge raporu alınamadı; sıfır olduğu anlamına gelmez."
    : "Henüz PC Edge raporu bulunmuyor."));

  const server: TransferEntry[] = [i.serverConfirmed === null
    ? empty("server-none", "Sunucudaki kayıtlar", "Sunucu sayacı alınamadı; sıfır olduğu anlamına gelmez.")
    : { id: "server-confirmed", title: "Doğrulanmış benzersiz kayıt",
        amount: fmt.num(i.serverConfirmed),
        detail: "Sunucu toplamı · PC Edge teslim sayısı ile aynı değildir. · " + ago(i.serverReadAt, i.now, "Son sorgu"),
        stale: old(i.serverReadAt, i.now) }];
  return [
    { id: "tracker", title: "1 · Tasma", entries: trackers },
    { id: "gateway", title: "2 · Gateway", entries: gateways },
    { id: "pc", title: "3 · PC Edge", entries: pc },
    { id: "server", title: "4 · Sunucu", entries: server },
  ];
}

// ---------------------------------------------------------------------------
// Aktarım sağlığı (sahip isteği 2026-11-11): her halka çalışıyor mu, kimde
// kaç kayıt bekliyor (son ölçüm zamanıyla). Sunucunun toplam sayısı yok;
// yalnız çalışıp çalışmadığı.
// ---------------------------------------------------------------------------
export type HealthTone = "fresh" | "aging" | "alarm" | "";
export interface HealthItem { id: string; title: string; line: string; tone: HealthTone; }
export interface HealthStage {
  id: "tracker" | "gateway" | "edge" | "server";
  title: string; line: string; tone: HealthTone; items: HealthItem[];
}
export interface HealthInput {
  now: number;
  trackers: { id: string; lastTime: number | null; periodS: number | null; missing?: NotArrived; noFix?: boolean }[];
  collars: readonly CollarStorageReading[];
  gateways: readonly FarmGateway[] | null;
  localGateway: { id: string | null; link: "ble" | "usb" | "none"; connected: boolean; held: number | null; capacity: number | null; readAt: number | null };
  edges: readonly EdgeQueue[] | null;
  phone: { pendingUpload: number | null };
  server: { signedIn: boolean; running: boolean; lastSync: number | null; error: string | null; backgroundSync?: number | null };
  label: (id: string) => string;
}

/** An Edge reports every ~10 s; two minutes of silence means it is not running. */
export const EDGE_ALIVE_MS = 2 * 60_000;
const HOUR = 3600_000;
const worst = (ts: HealthTone[]): HealthTone =>
  ts.includes("alarm") ? "alarm" : ts.includes("aging") ? "aging" : ts.includes("fresh") ? "fresh" : "";
const viaWord = (via: FarmGateway["via"]) =>
  via === "pc-usb" ? "PC Edge'e USB ile bağlı" : via === "pc-ble" ? "PC Edge'e Bluetooth ile bağlı"
    : via === "phone-usb" ? "bir telefona USB ile bağlı" : via === "phone-ble" ? "bir telefona Bluetooth ile bağlı" : "köprüye bağlı değil";

export function transferHealth(i: HealthInput): HealthStage[] {
  const now = i.now;
  // 1 · Tasmalar: kayıt geliyor mu; bellekteki kayıt, son okumasıyla.
  const readings = new Map(i.collars.map((c) => [c.device.toUpperCase(), c]));
  const trackerItems: HealthItem[] = i.trackers.map((t) => {
    const limit = Math.max(3 * (t.periodS ?? 900) * 1000, HOUR);
    const fresh = t.lastTime !== null && now - t.lastTime <= limit;
    const r = readings.get(t.id.toUpperCase());
    const parts = [t.lastTime !== null ? `son kaydı ${fmt.ago(t.lastTime, now)}` : "hiç kayıt gelmedi"];
    if (t.noFix) parts.push("GPS konum bulamıyor");
    const miss = t.missing?.missing ?? 0;
    if (miss) parts.push(`${fmt.num(miss)} kayıt henüz ulaşmadı${t.missing!.since !== null ? ` (${fmt.ago(t.missing!.since, now)} sonrası)` : ""}`);
    if (r) parts.push(`bellekte ${fmt.num(r.count)} kayıt (${r.via === "usb" ? "USB" : "Bluetooth"} ile ${fmt.ago(r.at, now)} okundu)`);
    const tone: HealthTone = fresh && !miss ? "fresh" : t.lastTime === null ? "" : "aging";
    return { id: t.id, title: i.label(t.id), line: parts.join(" · "), tone };
  }).sort((a, b) => Number(a.tone === "fresh") - Number(b.tone === "fresh") || a.title.localeCompare(b.title, "tr"));
  const freshN = i.trackers.filter((t) => t.lastTime !== null && now - t.lastTime <= Math.max(3 * (t.periodS ?? 900) * 1000, HOUR)).length;
  const missing = i.trackers.reduce((s, t) => s + (t.missing?.missing ?? 0), 0);
  const tracker: HealthStage = {
    id: "tracker", title: "Tasmalar", items: trackerItems,
    line: i.trackers.length
      ? `${freshN}/${i.trackers.length} tasmadan kayıt geliyor${missing ? ` · ${fmt.num(missing)} kayıt henüz ulaşmadı` : ""}`
      : "Henüz tasma yok",
    tone: !i.trackers.length ? "" : freshN === i.trackers.length ? (missing ? "aging" : "fresh") : freshN ? "aging" : "alarm",
  };

  // 2 · Gateway'ler: nasıl bağlı, kaç kayıt bekliyor (ne zaman ölçüldü), son gönderim.
  const gws = new Map<string, FarmGateway>();
  for (const g of i.gateways ?? []) gws.set(g.gateway.toUpperCase(), g);
  const L = i.localGateway, localId = L.connected && L.id ? L.id.toUpperCase() : null;
  if (localId) {
    const prev = gws.get(localId);
    gws.set(localId, { gateway: localId, via: L.link === "usb" ? "phone-usb" : "phone-ble",
      held: L.held ?? prev?.held ?? null, capacity: L.capacity ?? prev?.capacity ?? null,
      statusAt: L.held !== null && L.readAt !== null ? L.readAt : prev?.statusAt ?? null,
      lastSeen: now, lastUpload: prev?.lastUpload ?? null, trackers24h: prev?.trackers24h ?? 0 });
  }
  const gatewayItems: HealthItem[] = [...gws.values()].map((g) => {
    const mine = g.gateway === localId;
    const parts = [mine ? `bu telefona ${L.link === "usb" ? "USB" : "Bluetooth"} ile bağlı` : viaWord(g.via)];
    if (g.held !== null) parts.push(`${fmt.num(g.held)} kayıt bekliyor${g.statusAt !== null ? ` (${fmt.ago(g.statusAt, now)} ölçüldü)` : ""}`);
    if (g.lastUpload !== null) parts.push(`son gönderim ${fmt.ago(g.lastUpload, now)}`);
    else if (!g.via && g.lastSeen !== null) parts.push(`son görülme ${fmt.ago(g.lastSeen, now)}`);
    const full = g.held !== null && g.capacity ? g.held / g.capacity >= 0.5 : false;
    const tone: HealthTone = g.via ? (full ? "aging" : "fresh") : (g.held ?? 0) > 0 ? "alarm" : "aging";
    return { id: g.gateway, title: i.label(g.gateway), line: parts.join(" · "), tone };
  }).sort((a, b) => a.title.localeCompare(b.title, "tr"));
  const live = [...gws.values()].filter((g) => g.via).length;
  const held = [...gws.values()].reduce<number | null>((s, g) => (g.held === null ? s : (s ?? 0) + g.held), null);
  const gateway: HealthStage = {
    id: "gateway", title: "Gateway'ler", items: gatewayItems,
    line: !gws.size ? (i.gateways === null ? "Bilgi yok" : "Gateway görülmedi")
      : `${live}/${gws.size} bağlı${held !== null ? ` · ${fmt.num(held)} kayıt bekliyor` : ""}`,
    tone: !gws.size ? "" : worst(gatewayItems.map((x) => x.tone)),
  };

  // 3 · Köprü: PC Edge'ler çalışıyor mu (son raporu), bekleyen ve sorunlu; bu telefon.
  const edgeItems: HealthItem[] = (i.edges ?? []).map((e) => {
    const alive = now - e.lastSeen <= EDGE_ALIVE_MS;
    const parts = [alive ? `çalışıyor · son haber ${fmt.ago(e.lastSeen, now)}` : `çalışmıyor · ${fmt.ago(e.lastSeen, now)} haber yok`,
      `${fmt.num(e.pending)} kayıt gönderilecek`];
    if (e.quarantined) parts.push(`${fmt.num(e.quarantined)} sorunlu`);
    return { id: e.edge, title: e.edge, line: parts.join(" · "),
      tone: !alive ? "alarm" : e.quarantined ? "aging" : "fresh" } as HealthItem;
  }).sort((a, b) => a.title.localeCompare(b.title, "tr"));
  if (localId) edgeItems.push({
    id: "phone", title: "Bu telefon",
    line: `gateway'den alıyor${i.phone.pendingUpload ? ` · ${fmt.num(i.phone.pendingUpload)} kayıt gönderilecek` : " · gönderilecek kayıt yok"}`,
    tone: i.server.error && (i.phone.pendingUpload ?? 0) > 0 ? "aging" : "fresh",
  });
  const edgeAlive = edgeItems.filter((x) => x.tone !== "alarm").length;
  const edge: HealthStage = {
    id: "edge", title: "Köprü (Edge)", items: edgeItems,
    line: !edgeItems.length ? (i.edges === null ? "Bilgi alınamadı" : "Çalışan köprü yok")
      : `${edgeAlive}/${edgeItems.length} çalışıyor`,
    tone: !edgeItems.length ? (i.edges === null ? "" : "alarm") : worst(edgeItems.map((x) => x.tone)),
  };

  // 4 · Sunucu: yalnız çalışıyor mu.
  const S = i.server;
  const server: HealthStage = {
    id: "server", title: "Sunucu", items: [],
    line: !S.signedIn ? "Hesaba giriş yapılmadı" : S.error ? `Ulaşılamıyor · ${S.error}`
      : S.lastSync !== null ? `Çalışıyor · son eşitleme ${fmt.ago(S.lastSync, now)} · ${
          S.backgroundSync ? `izleme servisi en son ${fmt.ago(S.backgroundSync, now)} sordu` : "izleme servisi henüz sormadı"}`
      : S.running ? "Eşitleniyor…" : "Eşitleme bekleniyor",
    tone: !S.signedIn ? "" : S.error ? "alarm" : S.lastSync !== null && now - S.lastSync < 5 * 60_000 ? "fresh" : "aging",
  };
  return [tracker, gateway, edge, server];
}

/**
 * Tasmada (ya da yolda) bekleyen kayıtlar, tasmaya bağlanmadan: her kaydın
 * sıra numarası var; telefona ya da sunucuya ulaşanlar arasındaki boşluklar
 * henüz ulaşmamış kayıtlardır (canlı kayıt önce gelir, birikmişler sonra).
 * Yalnız en yeni kimlik dönemi (incarnation) ve son `days` gün; en yeni
 * kayıttan sonra yapılanlar bilinemez, sayı "en az"dır. Konumsuz kayıtlar da
 * sayılır (GPS bulamadığı rapor boşluk değildir).
 */
export interface NotArrived { missing: number; since: number | null; }

export function notArrived(recs: readonly { incarnation?: string; seq: number; time: string | null }[], now: number, days = 7): NotArrived {
  const from = now - days * 24 * HOUR;
  const byInc = new Map<string, { seq: number; t: number }[]>();
  for (const r of recs) {
    const t = r.time ? Date.parse(r.time) : NaN;
    if (!Number.isFinite(t) || t < from || t > now + 10 * 60_000 || !Number.isSafeInteger(r.seq)) continue;
    const k = r.incarnation ?? "";
    if (!byInc.has(k)) byInc.set(k, []);
    byInc.get(k)!.push({ seq: r.seq, t });
  }
  let newest: { seq: number; t: number }[] | null = null;
  for (const list of byInc.values())
    if (!newest || Math.max(...list.map((x) => x.t)) > Math.max(...newest.map((x) => x.t))) newest = list;
  if (!newest) return { missing: 0, since: null };
  const uniq = [...new Map(newest.map((x) => [x.seq, x])).values()].sort((a, b) => a.seq - b.seq);
  if (uniq.length < 2) return { missing: 0, since: null };
  let missing = 0, since: number | null = null;
  for (let k = 1; k < uniq.length; k++) {
    const gap = uniq[k].seq - uniq[k - 1].seq - 1;
    if (gap > 0) { missing += gap; if (since === null) since = uniq[k - 1].t; }
  }
  return { missing, since };
}
