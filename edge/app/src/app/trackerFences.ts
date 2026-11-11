// 3a: hangi tasmada hangi alanlar var (hesapta, telefonda bir kopya).
// Kayıt yalnız tasma alanları onaylayıp yeniden okununca yazılır.

export interface FenceRecord {
  digest: string; areas: number; vertices: number;
  zones: { id: string; name: string }[];
  appliedAt: number;   // ms
}

export interface FenceServer {
  trackerFences(): Promise<Record<string, FenceRecord>>;
  recordFence(device: string, r: Omit<FenceRecord, "appliedAt">): Promise<void>;
}

export type FenceState = "current" | "stale" | "never" | "none" | "clear";

/** none: this tracker gets no zones; never: has zones to get but none sent yet;
 * clear: it gets no zones but still holds some (removed in the app, not yet on the collar). */
export function fenceState(rec: FenceRecord | null, planDigest: string | null): FenceState {
  if (!planDigest) return rec && rec.areas > 0 ? "clear" : "none";
  if (!rec) return "never";
  return rec.digest === planDigest ? "current" : "stale";
}

type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.trackerFences.v1";
const ID16 = /^[0-9A-F]{16}$/;

function valid(v: unknown): Record<string, FenceRecord> {
  const out: Record<string, FenceRecord> = {};
  if (!v || typeof v !== "object") return out;
  for (const [k, r] of Object.entries(v as Record<string, unknown>)) {
    const x = r as Partial<FenceRecord> | null;
    if (ID16.test(k) && x && typeof x.digest === "string" && Number.isInteger(x.areas) &&
        Number.isInteger(x.vertices) && Array.isArray(x.zones) && Number.isFinite(x.appliedAt))
      out[k] = x as FenceRecord;
  }
  return out;
}

export class TrackerFenceBook {
  private records: Record<string, FenceRecord> = {};
  private pending: Record<string, FenceRecord> = {};
  private listeners = new Set<() => void>();
  private store: Store | null;

  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      const v = raw ? JSON.parse(raw) as { records?: unknown; pending?: unknown } : null;
      this.records = valid(v?.records);
      this.pending = valid(v?.pending);
    } catch { /* start empty */ }
  }

  private save(): void {
    try { this.store?.setItem(KEY, JSON.stringify({ records: this.records, pending: this.pending })); } catch { /* fine */ }
    for (const fn of this.listeners) fn();
  }

  subscribe(fn: () => void): () => void { this.listeners.add(fn); return () => { this.listeners.delete(fn); }; }
  snapshot(): Record<string, FenceRecord> { return this.records; }
  get(device: string): FenceRecord | null { return this.records[device.toUpperCase()] ?? null; }

  async set(device: string, rec: FenceRecord, server: FenceServer | null): Promise<void> {
    const dev = device.toUpperCase();
    this.records = { ...this.records, [dev]: rec };
    this.pending = { ...this.pending, [dev]: rec };
    this.save();
    if (server) await this.push(server, dev);
  }

  private async push(server: FenceServer, dev: string): Promise<void> {
    const rec = this.pending[dev];
    if (!rec) return;
    try {
      const { appliedAt: _ignored, ...body } = rec;
      await server.recordFence(dev, body);
      if (this.pending[dev] === rec) {
        const rest = { ...this.pending };
        delete rest[dev];
        this.pending = rest;
        this.save();
      }
    } catch { /* stays pending */ }
  }

  async refresh(server: FenceServer): Promise<void> {
    for (const dev of Object.keys(this.pending)) await this.push(server, dev);
    const remote = valid(await server.trackerFences());
    this.records = { ...remote, ...this.pending };
    this.save();
  }
}
