// ORUN S3 offline sync *candidate*: local mirror/outbox, not active login or upload.
// Never write to orun.app.zones.v1 / orun.app.zoneTargets.v1; these are the
// owner's originals and MUST survive migration, conflicts, and logout.
// Requires a separately enrolled identity; no shared bench token fallback.
import { canonicalE7, validateRing, type Zone } from "./zones.ts";
import type { TargetBook, ZoneGroup, ZoneRule } from "./zoneTargets.ts";

export type SyncKind = "zone" | "group" | "zone_target";
export type SyncAction = "UPSERT" | "DELETE";
export interface SyncOp {
  op_id: string;
  kind: SyncKind;
  id: string;
  action: SyncAction;
  base_revision: number;
  body: Record<string, unknown> | null;
}
export interface SyncChange {
  sequence: number;
  kind: SyncKind;
  id: string;
  revision: number;
  body: Record<string, unknown> | null;
  deleted: boolean;
}
export interface SyncResult {
  op_id: string;
  status: "ACCEPTED" | "CONFLICT" | "REJECTED";
  revision?: number;
  sequence?: number;
  reason?: string;
}
export interface SyncProblem { op: SyncOp; result: SyncResult; }
export interface SyncResource {
  revision: number;
  deleted: boolean;
  body: Record<string, unknown> | null;
}
export interface SyncState {
  schema: 1;
  site: string;
  installation: string;
  cursor: number;
  resources: Record<string, SyncResource>;
  outbox: SyncOp[];
  problems: SyncProblem[];
  /** true after the original draft was staged; NOT evidence of backend upload. */
  legacyStaged: boolean;
}

export interface SyncStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const id = (x: string): string => {
  if (!ID.test(x)) throw new Error("Senkronizasyon kaydı kimliği geçersiz.");
  return x;
};
const keyOf = (kind: SyncKind, resourceId: string): string => kind + ":" + resourceId;
const copy = <T>(x: T): T => JSON.parse(JSON.stringify(x)) as T;
function stableJson(x: unknown): string {
  if (Array.isArray(x)) return "[" + x.map(stableJson).join(",") + "]";
  if (x !== null && typeof x === "object") {
    return "{" + Object.keys(x).sort().map((k) =>
      JSON.stringify(k) + ":" + stableJson((x as Record<string, unknown>)[k])).join(",") + "}";
  }
  const json = JSON.stringify(x);
  if (json === undefined) throw new Error("Sunucu kaydı JSON biçiminde değil.");
  return json;
}
const int = (n: unknown): n is number =>
  typeof n === "number" && Number.isSafeInteger(n) && n >= 0;
const makeState = (site: string, installation: string): SyncState => ({
  schema: 1, site, installation, cursor: 0, resources: {}, outbox: [], problems: [], legacyStaged: false,
});
function validState(x: unknown, site: string, installation: string): x is SyncState {
  if (!x || typeof x !== "object" || Array.isArray(x)) return false;
  const s = x as Partial<SyncState>;
  if (s.schema !== 1 || s.site !== site || s.installation !== installation ||
      !int(s.cursor) || !s.resources || typeof s.resources !== "object" ||
      Array.isArray(s.resources) || !Array.isArray(s.outbox) ||
      !Array.isArray(s.problems) || typeof s.legacyStaged !== "boolean" ||
      s.outbox.length > 1000 || s.problems.length > 1000 ||
      Object.keys(s.resources).length > 10000) return false;
  const validOp = (o: SyncOp): boolean => !!o && typeof o === "object" &&
    typeof o.op_id === "string" && ID.test(o.op_id) &&
    (o.kind === "zone" || o.kind === "group" || o.kind === "zone_target") &&
    typeof o.id === "string" && ID.test(o.id) &&
    (o.action === "UPSERT" || o.action === "DELETE") && int(o.base_revision) &&
    ((o.action === "DELETE" && o.body === null) ||
     (o.action === "UPSERT" && !!o.body && typeof o.body === "object" && !Array.isArray(o.body)));
  const seen = new Set<string>();
  for (const o of s.outbox) {
    if (!validOp(o) || seen.has(o.op_id)) return false;
    seen.add(o.op_id);
  }
  for (const p of s.problems) {
    if (!p || !validOp(p.op) || !p.result ||
        !["CONFLICT", "REJECTED"].includes(p.result.status) ||
        p.result.op_id !== p.op.op_id || seen.has(p.op.op_id)) return false;
    seen.add(p.op.op_id);
  }
  for (const [k, r] of Object.entries(s.resources)) {
    if (!/^(zone|group|zone_target):[A-Za-z0-9._-]{1,128}$/.test(k) ||
        !r || !int(r.revision) || r.revision === 0 || typeof r.deleted !== "boolean" ||
        (r.deleted ? r.body !== null : !r.body || typeof r.body !== "object" || Array.isArray(r.body)))
      return false;
  }
  return true;
}

export function zoneBody(zone: Zone): Record<string, unknown> {
  id(zone.id);
  if (!zone.name.trim() || zone.name.trim().length > 40 || typeof zone.active !== "boolean" ||
      validateRing(zone.ring)) throw new Error("Alan verisi geçersiz; gönderim hazırlanmadı.");
  return { name: zone.name.trim(), active: zone.active,
    ring_e7: canonicalE7(zone.ring) };
}
export function groupBody(group: ZoneGroup): Record<string, unknown> {
  id(group.id);
  if (!group.name.trim() || group.name.trim().length > 60 ||
      group.devices.length > 512 || new Set(group.devices).size !== group.devices.length)
    throw new Error("Grup geçersiz.");
  for (const d of group.devices) id(d);
  return { name: group.name.trim(), device_ids: [...group.devices].sort() };
}
export function targetBody(zoneId: string, rule: ZoneRule): Record<string, unknown> {
  id(zoneId);
  if (!rule || (rule.scope !== "selected" && rule.scope !== "all") ||
      !Array.isArray(rule.devices) || !Array.isArray(rule.groups) ||
      rule.devices.length > 512 || rule.groups.length > 100 ||
      new Set(rule.devices).size !== rule.devices.length ||
      new Set(rule.groups).size !== rule.groups.length ||
      (rule.scope === "all" && (rule.devices.length > 0 || rule.groups.length > 0)))
    throw new Error("Hedef seçimi geçersiz.");
  for (const x of [...rule.devices, ...rule.groups]) id(x);
  return { zone_id: zoneId, scope: rule.scope,
    device_ids: [...rule.devices].sort(), group_ids: [...rule.groups].sort() };
}

/** One storage key per enrolled installation+site. Caller must provide stable
 * enrollment identifiers; neither a phone-generated device ID nor JWT claim
 * is by itself proof that the user has server permissions. */
export class OfflineSyncStore {
  private readonly storage: SyncStorage;
  private readonly storageKey: string;
  private state: SyncState;
  readonly warning: string | null;

  constructor(storage: SyncStorage, storageKey: string, site: string, installation: string) {
    id(site); id(installation);
    this.storage = storage;
    this.storageKey = storageKey;
    let warning: string | null = null;
    let state = makeState(site, installation);
    try {
      const raw = storage.getItem(storageKey);
      if (raw !== null) {
        const parsed: unknown = JSON.parse(raw);
        if (!validState(parsed, site, installation)) throw new Error("corrupt mirror");
        state = parsed;
      }
    } catch {
      warning = "Eşitleme kaydı okunamadı: orijinal alanlar korunuyor; eşitleme durduruldu.";
    }
    this.state = state;
    this.warning = warning;
  }
  get(): SyncState { return copy(this.state); }
  private write(next: SyncState): void {
    if (this.warning) throw new Error(this.warning);
    if (!validState(next, this.state.site, this.state.installation))
      throw new Error("Senkronizasyon taslağı doğrulanamadı.");
    // Storage failure must not make an operation appear queued, accepted or
    // synchronized. Both mirror and outbox persist as ONE document.
    this.storage.setItem(this.storageKey, JSON.stringify(next));
    this.state = next;
  }
  private op(next: SyncState, kind: SyncKind, rid: string, action: SyncAction,
             body: Record<string, unknown> | null, opId: string): void {
    id(rid); id(opId);
    const key = keyOf(kind, rid);
    if ([...next.outbox, ...next.problems.map((p) => p.op)].some((o) => keyOf(o.kind, o.id) === key))
      throw new Error("Bu kayıt zaten bekliyor veya çakışmada; üzerine yazılmayacak.");
    if (next.outbox.some((o) => o.op_id === opId) || next.problems.some((p) => p.op.op_id === opId))
      throw new Error("İşlem kimliği daha önce kullanıldı.");
    const resource = next.resources[key];
    if (resource?.deleted) throw new Error("Silinen alan kimliği yeniden kullanılamaz.");
    if (action === "DELETE" && !resource)
      throw new Error("Sunucuya henüz kabul edilmemiş kayıt silinemez.");
    if (next.outbox.length >= 1000) throw new Error("Çevrimdışı kuyruk dolu.");
    next.outbox.push({ op_id: opId, kind, id: rid, action,
      base_revision: resource?.revision ?? 0, body: copy(body) });
  }
  /** Explicit, one-time stage of the owner's ORIGINAL local drafts.
   * Never rewrites original stores and never claims network/server success.
   * All-or-nothing: validation completes before a single persistent write. */
  stageLegacy(zones: Zone[], targets: TargetBook, freshId: () => string): void {
    const next = this.get();
    if (next.legacyStaged || next.outbox.length || next.problems.length ||
        next.cursor !== 0 || Object.keys(next.resources).length)
      throw new Error("Bu kurulumda ilk alan aktarımı zaten hazırlandı.");
    if (zones.length > 100 || targets.groups.length > 100 ||
        Object.keys(targets.rules).length > 200)
      throw new Error("Yerel taslaklar kapasiteyi aşıyor.");
    const zoneIds = new Set(zones.map((z) => z.id));
    const groupIds = new Set(targets.groups.map((g) => g.id));
    if (zoneIds.size !== zones.length || groupIds.size !== targets.groups.length)
      throw new Error("Tekrarlanan alan/grup kimliği var.");
    for (const z of zones) this.op(next, "zone", z.id, "UPSERT", zoneBody(z), freshId());
    for (const g of targets.groups) this.op(next, "group", g.id, "UPSERT", groupBody(g), freshId());
    for (const [zoneId, rule] of Object.entries(targets.rules)) {
      if (!zoneIds.has(zoneId) || rule.groups.some((g) => !groupIds.has(g)))
        throw new Error("Hedef taslağında bulunamayan alan veya grup var.");
      this.op(next, "zone_target", zoneId, "UPSERT", targetBody(zoneId, rule), freshId());
    }
    next.legacyStaged = true;
    this.write(next);
  }
  /** New edits can be queued only after canonical base revision is known.
   * Existing UI remains local-only until a separately tested store adapter
   * commits local drafts and outbox in one durable transaction. */
  queue(kind: SyncKind, rid: string, body: Record<string, unknown> | null,
        opId: string): void {
    const next = this.get();
    const action = body === null ? "DELETE" : "UPSERT";
    this.op(next, kind, rid, action, body, opId);
    this.write(next);
  }
  nextBatch(max = 50): SyncOp[] {
    if (!Number.isSafeInteger(max) || max < 1 || max > 50) throw new Error("Geçersiz paket.");
    return copy(this.state.outbox.slice(0, max));
  }
  /** Only an authenticated server response may acknowledge operations.
   * A mismatch or unexpected ID never erases queued user data. */
  recordResults(results: SyncResult[]): void {
    const next = this.get();
    if (!Array.isArray(results) || results.length > 50) throw new Error("Geçersiz sunucu yanıtı.");
    const known = new Map(next.outbox.map((o) => [o.op_id, o]));
    const seen = new Set<string>();
    for (const r of results) {
      if (!r || typeof r.op_id !== "string" || seen.has(r.op_id) ||
          !known.has(r.op_id) || !["ACCEPTED", "CONFLICT", "REJECTED"].includes(r.status))
        throw new Error("Sunucu yanıtı eşleşmiyor; kuyruk korundu.");
      seen.add(r.op_id);
      const op = known.get(r.op_id)!;
      if (r.status === "ACCEPTED") {
        if (!int(r.revision) || r.revision !== op.base_revision + 1 ||
            !int(r.sequence) || r.sequence === 0) throw new Error("Kabul kanıtı geçersiz.");
        const k = keyOf(op.kind, op.id);
        const old = next.resources[k];
        if (old && old.revision > r.revision)
          throw new Error("Daha yeni sunucu sürümü kaybedilemez.");
        next.resources[k] = {
          revision: r.revision, deleted: op.action === "DELETE",
          body: op.action === "UPSERT" ? copy(op.body) : null,
        };
      } else {
        next.problems.push({ op, result: copy(r) });
      }
    }
    next.outbox = next.outbox.filter((o) => !seen.has(o.op_id));
    this.write(next);
  }
  /** Pull server-canonical changes; never modifies original phone drafts.
   * Cursor and resource materialization persist atomically. */
  applyPull(changes: SyncChange[], cursor: number): void {
    const next = this.get();
    if (!Array.isArray(changes) || changes.length > 200 ||
        !int(cursor) || cursor < next.cursor)
      throw new Error("Sunucu değişiklik dizisi geçersiz.");
    // An empty page must never let a server response skip unseen changes.
    if (!changes.length && cursor !== next.cursor)
      throw new Error("Boş sunucu sayfası cursor atlayamaz.");
    let last = next.cursor;
    for (const c of changes) {
      if (!c || !int(c.sequence) || c.sequence <= last || c.sequence > cursor ||
          !int(c.revision) || c.revision < 1 || typeof c.id !== "string" ||
          !ID.test(c.id) || !["zone", "group", "zone_target"].includes(c.kind) ||
          typeof c.deleted !== "boolean" ||
          (c.deleted ? c.body !== null : !c.body || typeof c.body !== "object" || Array.isArray(c.body)))
        throw new Error("Sunucudan tutarsız değişiklik; cursor ilerletilmedi.");
      const k = keyOf(c.kind, c.id);
      const old = next.resources[k];
      if (!old || c.revision > old.revision) {
        next.resources[k] = { revision: c.revision, deleted: c.deleted,
          body: copy(c.body) };
      } else if (c.revision === old.revision &&
                 (c.deleted !== old.deleted || stableJson(c.body) !== stableJson(old.body))) {
        throw new Error("Aynı revizyonda farklı içerik; cursor korunuyor.");
      }
      last = c.sequence;
    }
    if (changes.length && last !== cursor)
      throw new Error("Cursor son sunucu kaydıyla eşleşmiyor.");
    next.cursor = cursor;
    this.write(next);
  }
}
