// Small persisted stores for what the owner sets on this phone: animal
// profiles + collar assignments, app preferences, station positions and
// "Gördüm" marks. localStorage, wrapped: a private/blocked storage only makes
// them memory-only, never breaks the app. Moving profiles to the server so
// every phone shares them is later work (step 3C+).
import type { LatLon } from "./geo.ts";
import type { AssetProfile, Assignment } from "./model.ts";

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
  animals: boolean; trail: boolean; heat: boolean; coverage: boolean; zones: boolean;
  stations: boolean; me: boolean; labels: boolean; relief: boolean;
}
export interface Prefs {
  theme: Theme; sun: boolean; demo: boolean | null;   // null: not chosen yet
  base: BaseMap; tilt: boolean; satelliteSafe: boolean; layers: Layers; range: Range;
}

export const DEFAULT_PREFS: Prefs = {
  theme: "auto", sun: false, demo: null, base: "satellite", tilt: false, satelliteSafe: false,
  layers: { animals: true, trail: true, heat: false, coverage: false, zones: true, stations: true, me: true, labels: true, relief: false },
  range: "h24",
};

export function rangeStart(r: Range, now: number): number {
  if (r === "today") { const d = new Date(now); d.setHours(0, 0, 0, 0); return d.getTime(); }
  const h = { h24: 24, d3: 72, d7: 168, d30: 720 }[r];
  return now - h * 3600_000;
}

export const prefs = new Persisted<Prefs>("orun.app.prefs.v1", () => ({ ...DEFAULT_PREFS }),
  (raw) => (raw && typeof raw === "object"
    ? { ...DEFAULT_PREFS, ...(raw as Partial<Prefs>), layers: { ...DEFAULT_PREFS.layers, ...((raw as Partial<Prefs>).layers ?? {}) } }
    : null));

// ------------------------------------------- station positions and acks --
export const stationPlaces = new Persisted<Record<string, LatLon>>("orun.app.stations.v1", () => ({}));
export const seenEvents = new Persisted<string[]>("orun.app.seen.v1", () => []);
