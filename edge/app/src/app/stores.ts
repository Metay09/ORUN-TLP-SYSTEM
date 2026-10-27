// Small persisted stores for what the owner sets on this phone: animal
// profiles + collar assignments, app preferences, station positions and
// "Gördüm" marks. localStorage, wrapped: a private/blocked storage only makes
// them memory-only, never breaks the app. Moving profiles to the server so
// every phone shares them is later work (step 3C+).
import type { LatLon } from "./geo.ts";
import type { AssetProfile, Assignment } from "./model.ts";
import { NO_GRAZE, checkSet, validateRing, type Graze, type LonLat, type Zone, type ZoneBook } from "./zones.ts";

type Fn = () => void;

export class Persisted<T> {
  private value: T;
  private listeners = new Set<Fn>();
  private key: string | null;
  constructor(key: string | null, fallback: () => T, migrate?: (raw: unknown) => T | null) {
    this.key = key;
    let v: T | null = null;
    if (key) {
      try {
        const raw = localStorage.getItem(key);
        if (raw !== null) v = migrate ? migrate(JSON.parse(raw)) : (JSON.parse(raw) as T);
      } catch { v = null; }
    }
    this.value = v ?? fallback();
  }
  get(): T { return this.value; }
  set(next: T): void {
    this.value = next;
    if (this.key) {
      try { localStorage.setItem(this.key, JSON.stringify(next)); } catch { /* memory only */ }
    }
    for (const fn of this.listeners) fn();
  }
  update(fn: (v: T) => T): void { this.set(fn(this.value)); }
  subscribe = (fn: Fn): (() => void) => {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  };
}

// ---------------------------------------------------------------- assets --
export const SWATCHES = ["#2E8B57", "#B4475C", "#3D7EA6", "#A57A1E", "#6D5BD0", "#C2611F", "#5E8F8A", "#5B6B2E", "#8A4F9A", "#2F6F8F"];

export function colorFor(id: string): string {
  let h = 0;
  for (const c of id) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return SWATCHES[h % SWATCHES.length];
}

export function blankProfile(id: string): AssetProfile {
  return { id, kind: "animal", name: "", color: colorFor(id), tag: "", breed: "", sex: "", birth: "", notes: [] };
}

export interface AssetBook { assets: Record<string, AssetProfile>; assignments: Assignment[]; }

const LEGACY_KEY = "orun.edge.animals";   // UX v2 profiles: { [collar]: {name,color,note} }

function fromLegacy(): AssetBook {
  const book: AssetBook = { assets: {}, assignments: [] };
  try {
    const old = JSON.parse(localStorage.getItem(LEGACY_KEY) ?? "{}") as Record<string, { name?: string; color?: string; note?: string }>;
    for (const [id, p] of Object.entries(old ?? {})) {
      const a = blankProfile(id);
      a.name = (p.name ?? "").trim();
      if (p.color) a.color = p.color;
      if (p.note?.trim()) a.notes = [{ at: Date.now(), text: p.note.trim() }];
      book.assets[id] = a;
      book.assignments.push({ device: id, asset: id, from: 0 });
    }
  } catch { /* nothing to carry over */ }
  return book;
}

export class AssetStore extends Persisted<AssetBook> {
  constructor(key: string | null, seed?: AssetBook) {
    super(key, () => seed ?? (key ? fromLegacy() : { assets: {}, assignments: [] }),
      (raw) => (raw && typeof raw === "object" && "assets" in raw ? raw as AssetBook : null));
  }

  /** Asset a collar belonged to at `time` (ms). Unassigned collars stand for
   *  their own implicit animal, keyed by the collar id. */
  resolver(): (device: string, time: number) => string {
    const by = new Map<string, Assignment[]>();
    for (const a of this.get().assignments) {
      if (!by.has(a.device)) by.set(a.device, []);
      by.get(a.device)!.push(a);
    }
    for (const list of by.values()) list.sort((a, b) => a.from - b.from);
    return (device, time) => {
      const list = by.get(device);
      if (!list) return device;
      let pick: Assignment | null = null;
      for (const a of list) if (a.from <= time) pick = a;
      return pick ? pick.asset : list[0].asset;
    };
  }

  profile(id: string): AssetProfile {
    return this.get().assets[id] ?? blankProfile(id);
  }

  save(p: AssetProfile, device: string | null): void {
    this.update((b) => {
      const clean: AssetProfile = {
        ...p, name: p.name.trim().slice(0, 40), tag: p.tag.trim().slice(0, 40), breed: p.breed.trim().slice(0, 40),
        notes: p.notes.slice(-200).map((n) => ({ at: n.at, text: n.text.slice(0, 500) })),
      };
      const assignments = device && !b.assignments.some((a) => a.device === device)
        ? [...b.assignments, { device, asset: p.id, from: 0 }] : b.assignments;
      return { assets: { ...b.assets, [p.id]: clean }, assignments };
    });
  }

  addNote(id: string, device: string | null, text: string): void {
    const t = text.trim();
    if (!t) return;
    const p = this.profile(id);
    this.save({ ...p, notes: [...p.notes, { at: Date.now(), text: t }] }, device);
  }
}

// ----------------------------------------------------------------- prefs --
export type Theme = "auto" | "light" | "dark";
export type BaseMap = "satellite" | "terrain" | "street";
export type Range = "today" | "h24" | "d3" | "d7" | "d30";
export interface Layers {
  animals: boolean; trail: boolean; heat: boolean; coverage: boolean;
  stations: boolean; me: boolean; labels: boolean; relief: boolean;
  zones: boolean; herdTrails: boolean; links: boolean;
}
export type ZoneFill = "none" | "light" | "hatch";
/** Satellite imagery: Esri's current mosaic or one Wayback release. */
export interface SatPrefs {
  kind: "esri" | "wayback";
  release: string;    // Wayback release id
  maxZoom: number;    // highest tile level asked for; above it tiles are enlarged
}
export interface Prefs {
  theme: Theme; sun: boolean; demo: boolean | null;   // null: not chosen yet
  base: BaseMap; tilt: boolean; layers: Layers; range: Range;
  sat: SatPrefs; zoneFill: ZoneFill;
}

// Esri has no 18-19 imagery over most of rural Aydın; asking for it returns
// "Map data not yet available" pictures. 17 is sharp there and is enlarged
// beyond (settable 14-19 in the layer panel).
export const DEFAULT_SAT: SatPrefs = { kind: "esri", release: "26334", maxZoom: 17 };

export const DEFAULT_PREFS: Prefs = {
  theme: "auto", sun: false, demo: null, base: "satellite", tilt: false,
  layers: {
    animals: true, trail: true, heat: false, coverage: false, stations: true, me: true, labels: true, relief: false,
    zones: true, herdTrails: false, links: false,
  },
  range: "h24", sat: DEFAULT_SAT, zoneFill: "light",
};

export function rangeStart(r: Range, now: number): number {
  if (r === "today") { const d = new Date(now); d.setHours(0, 0, 0, 0); return d.getTime(); }
  const h = { h24: 24, d3: 72, d7: 168, d30: 720 }[r];
  return now - h * 3600_000;
}

export const prefs = new Persisted<Prefs>("orun.app.prefs.v1", () => ({ ...DEFAULT_PREFS }),
  (raw) => (raw && typeof raw === "object"
    ? {
      ...DEFAULT_PREFS, ...(raw as Partial<Prefs>),
      layers: { ...DEFAULT_PREFS.layers, ...((raw as Partial<Prefs>).layers ?? {}) },
      sat: { ...DEFAULT_SAT, ...((raw as Partial<Prefs>).sat ?? {}) },
    }
    : null));

// ------------------------------------------- station positions and acks --
export const stationPlaces = new Persisted<Record<string, LatLon>>("orun.app.stations.v1", () => ({}));
export const seenEvents = new Persisted<string[]>("orun.app.seen.v1", () => []);

// ----------------------------------------------------------------- zones --
const LEGACY_ZONE_KEY = "orun.fences.v1"; // HTML prototype and PR #180

interface LegacyMigration { zones: Zone[]; error: string | null; }
function legacyDrafts(): LegacyMigration {
  try {
    const raw = localStorage.getItem(LEGACY_ZONE_KEY);
    if (raw === null) return { zones: [], error: null };
    const value: unknown = JSON.parse(raw);
    if (!Array.isArray(value) || value.length > 100)
      throw new Error("Eski mera listesi geçersiz.");
    const unique = new Set<string>();
    const zones: Zone[] = value.map((record: unknown) => {
      if (!record || typeof record !== "object" || Array.isArray(record))
        throw new Error("Eski mera kaydı geçersiz.");
      const z = record as Record<string, unknown>;
      if (typeof z.id !== "string" || !z.id || unique.has(z.id) ||
          typeof z.name !== "string" || !z.name.trim() ||
          (z.active !== undefined && typeof z.active !== "boolean") ||
          !Array.isArray(z.ring) || z.ring.length > 65)
        throw new Error("Eski meranın kimliği, adı veya köşeleri geçersiz.");
      unique.add(z.id);
      const ring = z.ring.map((point: unknown): LonLat => {
        if (!Array.isArray(point) || point.length !== 2 ||
            point.some((c: unknown) => typeof c !== "number" || !Number.isFinite(c)) ||
            point[0] <= -180 || point[0] >= 180 || point[1] <= -90 || point[1] >= 90)
          throw new Error("Eski mera koordinatları geçersiz.");
        return [point[0], point[1]];
      });
      // Legacy and firmware allow an explicitly repeated closing point.
      if (ring.length > 1 && ring[0][0] === ring[ring.length - 1][0] &&
          ring[0][1] === ring[ring.length - 1][1]) ring.pop();
      if (validateRing(ring)) throw new Error("Eski mera çokgeni geçersiz.");
      return { id: z.id, name: z.name.trim(), active: z.active !== false, ring, updated: Date.now() };
    });
    if (!checkSet(zones).ok) throw new Error("Eski meralar tasma sınırlarını aşıyor.");
    return { zones, error: null };
  } catch {
    // NEVER write to the old key, and never silently overwrite corrupt legacy
    // drafts with an empty new document.
    return { zones: [], error: "Eski mera kayıtları güvenle okunamadı. Veri kaybını önlemek için alan değişiklikleri durduruldu; eski uygulama verilerini silmeyin." };
  }
}

/** A persisted legacy draft is restored when no new-format record exists.
 * The old key is retained unchanged for downgrade/backup. */
export class ZoneStore extends Persisted<ZoneBook> {
  readonly migrationWarning: string | null;
  private readonly storageKey: string | null;
  constructor(key: string | null, seed?: Zone[]) {
    let invalidExisting = false;
    const existing = (() => {
      if (!key) return false;
      try {
        const raw = localStorage.getItem(key);
        if (raw === null) return false;
        const document: unknown = JSON.parse(raw);
        if (!document || typeof document !== "object" || Array.isArray(document) ||
            !Array.isArray((document as ZoneBook).zones)) throw new Error("Invalid zone book");
        const zones = (document as ZoneBook).zones;
        if (zones.length > 100 || new Set(zones.map((z) => z?.id)).size !== zones.length ||
            zones.some((z) => !z || typeof z.id !== "string" || !z.id ||
              typeof z.name !== "string" || !z.name.trim() ||
              typeof z.active !== "boolean" || !Array.isArray(z.ring) ||
              z.ring.some((point) => !Array.isArray(point) || point.length !== 2 ||
                point.some((v) => typeof v !== "number" || !Number.isFinite(v))) ||
              validateRing(z.ring)) ||
            !checkSet(zones).ok) throw new Error("Invalid zone geometry");
        return true;
      } catch {
        invalidExisting = true;
        return true; // never silently replace a corrupt new record from legacy
      }
    })();
    const legacy = key && !existing && !seed ? legacyDrafts() : { zones: [], error: null };
    super(key, () => ({ zones: seed ?? legacy.zones, graze: { ...NO_GRAZE } }),
      (raw) => (raw && typeof raw === "object" && !Array.isArray(raw) &&
        Array.isArray((raw as ZoneBook).zones)
        ? { zones: (raw as ZoneBook).zones, graze: { ...NO_GRAZE, ...((raw as ZoneBook).graze ?? {}) } } : null));
    this.storageKey = key;
    this.migrationWarning = invalidExisting
      ? "Yeni mera kayıtları bozuk veya desteklenmiyor; orijinal veriler korunuyor, üzerine yazılmayacak."
      : legacy.error;
  }

  private commit(book: ZoneBook): void {
    if (this.migrationWarning) throw new Error(this.migrationWarning);
    // Persist before reporting any user-visible success: Persisted.set()
    // intentionally tolerates private/blocked storage for unrelated settings,
    // but a boundary must never be acknowledged if its write failed.
    if (this.storageKey) localStorage.setItem(this.storageKey, JSON.stringify(book));
    this.set(book);
  }

  /** Atomically refuse a malformed or over-budget set, including imports.
   * Avoid reporting a saved area for a refused transaction. */
  private checked(zones: Zone[]): Zone[] {
    if (this.migrationWarning) throw new Error(this.migrationWarning);
    if (zones.length > 100 || new Set(zones.map((z) => z.id)).size !== zones.length)
      throw new Error("Mera kaydında tekrarlanan kimlik veya fazla alan var.");
    for (const z of zones) {
      if (!z.id || !z.name.trim() || validateRing(z.ring))
        throw new Error(`${z.name || "Mera"}: geçersiz alan sınırı.`);
    }
    const check = checkSet(zones);
    if (!check.ok) throw new Error(check.errors.join(" "));
    return zones;
  }
  save(z: Zone): void {
    const b = this.get();
    const clean: Zone = { ...z, name: z.name.trim().slice(0, 40) || "Alan", updated: Date.now() };
    const i = b.zones.findIndex((x) => x.id === z.id);
    const zones = i >= 0 ? b.zones.map((x, k) => (k === i ? clean : x)) : [...b.zones, clean];
    this.commit({ ...b, zones: this.checked(zones) });
  }
  remove(id: string): void {
    const b = this.get();
    this.commit({ ...b, zones: b.zones.filter((z) => z.id !== id) });
  }
  add(list: Zone[]): void {
    const b = this.get();
    this.commit({ ...b, zones: this.checked([...b.zones, ...list]) });
  }
  // Preserved as storage schema data only. No UI/alarms until secure collar
  // command and ACK protocol for FREE_GRAZE is implemented.
  setGraze(g: Graze): void { this.commit({ ...this.get(), graze: g }); }
}
