// Local targeting plan for pasture drafts. NOT a device command or an ACK.
// Explicit opt-in: new/legacy zones target no device until the owner selects it.
// The actual per-device REPLACE_SET/CLEAR_SET requires an authenticated writer.
import { checkSet, type Zone } from "./zones.ts";

export interface ZoneGroup {
  id: string;
  name: string;
  devices: string[]; // ORUN device IDs, never animal names or legacy roles
}
export interface ZoneRule {
  scope: "all" | "selected";
  devices: string[];
  groups: string[];  // locally defined group IDs
}
export interface TargetBook {
  groups: ZoneGroup[];
  rules: Record<string, ZoneRule>; // zone ID -> intended targets
}
export interface DeviceZonePlan {
  device: string;
  zones: Zone[];
  errors: string[];
}

export const blankTargets = (): TargetBook => ({ groups: [], rules: {} });
export const emptyRule = (): ZoneRule => ({ scope: "selected", devices: [], groups: [] });

function identifier(x: unknown): x is string {
  return typeof x === "string" && x.length > 0 && x.length <= 128 &&
    /^[A-Za-z0-9._-]+$/.test(x);
}
function ids(x: unknown): x is string[] {
  return Array.isArray(x) && x.length <= 512 && x.every(identifier) &&
    new Set(x).size === x.length;
}
function validBook(x: unknown): x is TargetBook {
  if (!x || typeof x !== "object" || Array.isArray(x)) return false;
  const b = x as Partial<TargetBook>;
  if (!Array.isArray(b.groups) || b.groups.length > 100 || !b.rules ||
      typeof b.rules !== "object" || Array.isArray(b.rules)) return false;
  const seen = new Set<string>();
  for (const g of b.groups) {
    if (!g || !identifier(g.id) || seen.has(g.id) ||
        typeof g.name !== "string" || !g.name.trim() || g.name.length > 60 ||
        !ids(g.devices)) return false;
    seen.add(g.id);
  }
  const rules = Object.entries(b.rules);
  if (rules.length > 200) return false;
  return rules.every(([zoneId, r]) =>
    identifier(zoneId) && !!r && (r.scope === "all" || r.scope === "selected") &&
    ids(r.devices) && ids(r.groups) && r.groups.every((id) => seen.has(id)));
}

/** Explicitly selected targets; all targets are resolved against known devices. */
export function targetsForZone(book: TargetBook, zoneId: string, known: string[]): string[] {
  const rule = book.rules[zoneId];
  if (!rule) return [];
  const available = new Set(known);
  if (rule.scope === "all") return [...available].sort();
  const selected = new Set(rule.devices);
  for (const g of book.groups) if (rule.groups.includes(g.id))
    for (const id of g.devices) selected.add(id);
  return [...selected].filter((id) => available.has(id)).sort();
}

/** Build the FULL replacement set separately for each selected device.
 * Empty result is never an implicit CLEAR command. */
export function zonePlanForDevice(book: TargetBook, zones: Zone[], device: string,
                                  knownDevices: string[]): DeviceZonePlan {
  const selected = zones.filter((z) =>
    z.active && targetsForZone(book, z.id, knownDevices).includes(device));
  const check = checkSet(selected);
  return { device, zones: selected, errors: check.errors };
}

type Listener = () => void;

/** Persist first, publish second. Corrupt existing data is NEVER overwritten. */
export class ZoneTargetStore {
  private book: TargetBook = blankTargets();
  private listeners = new Set<Listener>();
  private readonly key: string | null;
  readonly warning: string | null;
  constructor(key: string | null) {
    this.key = key;
    let warning: string | null = null;
    if (key) {
      try {
        const raw = localStorage.getItem(key);
        if (raw !== null) {
          const decoded: unknown = JSON.parse(raw);
          if (!validBook(decoded)) throw new Error("invalid target book");
          this.book = decoded;
        }
      } catch {
        warning = "Alan hedefleri okunamadı. Eski veriler korunuyor; hedefleri değiştirmeyin.";
      }
    }
    this.warning = warning;
  }
  get = (): TargetBook => this.book;
  subscribe = (fn: Listener): (() => void) => {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  };
  private commit(book: TargetBook): void {
    if (this.warning) throw new Error(this.warning);
    if (!validBook(book)) throw new Error("Hedef listesi geçersiz.");
    if (this.key) localStorage.setItem(this.key, JSON.stringify(book));
    this.book = book;
    for (const fn of this.listeners) fn();
  }
  saveGroup(group: ZoneGroup): void {
    const clean = { ...group, name: group.name.trim(), devices: [...group.devices] };
    const groups = this.book.groups.filter((g) => g.id !== group.id);
    this.commit({ ...this.book, groups: [...groups, clean] });
  }
  removeGroup(id: string): void {
    const rules = Object.fromEntries(Object.entries(this.book.rules).map(([zoneId, r]) =>
      [zoneId, { ...r, groups: r.groups.filter((g) => g !== id) }]));
    this.commit({ groups: this.book.groups.filter((g) => g.id !== id), rules });
  }
  saveRule(zoneId: string, rule: ZoneRule): void {
    this.commit({ ...this.book, rules: { ...this.book.rules,
      [zoneId]: { ...rule, devices: [...rule.devices], groups: [...rule.groups] } } });
  }
  removeZone(zoneId: string): void {
    const rules = { ...this.book.rules };
    delete rules[zoneId];
    this.commit({ ...this.book, rules });
  }
}
