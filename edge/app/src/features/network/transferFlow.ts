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
      statusAt: freshRead ? i.now : prev?.statusAt ?? null,
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
