// İşlemler (sahip kararı 2026-11-02): alan ve ayar gönderme kaydı. Her
// "uygula/kaydet" bir işlemdir; her hedef cihaz için ayrı durum tutulur.
// "Tasmaya işlendi" YALNIZ cihazın kendi onayıyla yazılır (USB/Bluetooth sonuç
// satırı, LoRa'da tasmanın onayı). Ara adım için fazladan telsiz trafiği yok:
// "Gateway'de" yalnız LoRa yolunda, gateway'in zaten gönderdiği bilgiden.
import type { LonLat } from "./zones.ts";
import { SETTINGS } from "./deviceSettings.ts";

export type OpKind = "fence" | "settings";
export type OpState = "waiting" | "gateway" | "applied" | "same" | "rejected" | "superseded";
export type OpVia = "usb" | "ble" | "lora";

export interface FencePayload {
  digest: string; areas: number; vertices: number;
  zones: { id: string; name: string; ring: LonLat[] }[];
}
export interface SettingsPayload { set: Record<string, number | boolean>; }

export interface OpTarget {
  op: string; device: string; kind: OpKind;
  payload: FencePayload | SettingsPayload;
  state: OpState; createdAt: number; updatedAt: number;
  via: OpVia | null; note: string | null;
}

export const STATE_WORD: Record<OpState, string> = {
  waiting: "Sırada", gateway: "Gateway'de", applied: "Tasmaya işlendi", same: "Zaten güncel",
  rejected: "Reddedildi", superseded: "Yerine yenisi geldi",
};
export const OPEN: OpState[] = ["waiting", "gateway"];
export const KIND_WORD: Record<OpKind, string> = { fence: "Alanlar", settings: "Ayarlar" };

export function newOpId(now = Date.now()): string {
  return "op-" + now.toString(36) + Math.floor(Math.random() * 36 ** 6).toString(36).padStart(6, "0");
}

/** Steps for the progress bar: USB/Bluetooth skip the gateway. */
export function steps(t: OpTarget): { label: string; on: boolean; bad?: boolean }[] {
  const done = t.state === "applied" || t.state === "same";
  const out: { label: string; on: boolean; bad?: boolean }[] = [{ label: "Sırada", on: true }];
  if (t.via === "lora" || t.state === "gateway") out.push({ label: "Gateway'de", on: t.state !== "waiting" });
  out.push({ label: t.state === "rejected" ? "Reddedildi" : "İşlendi", on: done || t.state === "rejected", bad: t.state === "rejected" });
  return out;
}

export interface OpsServer {
  deviceOps(): Promise<OpTarget[]>;
  createOp(op: string, kind: OpKind, items: { device: string; payload: OpTarget["payload"] }[]): Promise<void>;
  opResult(op: string, device: string, state: Exclude<OpState, "waiting" | "superseded">, via: OpVia, note?: string): Promise<void>;
}

type Pending =
  | { kind: "create"; op: string; opKind: OpKind; items: { device: string; payload: OpTarget["payload"] }[] }
  | { kind: "result"; op: string; device: string; state: Exclude<OpState, "waiting" | "superseded">; via: OpVia; note?: string };
type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.deviceOps.v1";
const MAX = 500;

function valid(t: unknown): t is OpTarget {
  const x = t as OpTarget;
  return !!x && typeof x.op === "string" && /^[0-9A-F]{16}$/.test(x.device) && (x.kind === "fence" || x.kind === "settings")
    && !!x.payload && typeof x.payload === "object" && typeof x.state === "string" && Number.isFinite(x.createdAt);
}

/** Phone copy of the account's operations; made offline, they wait here. */
export class DeviceOpsBook {
  private rows: OpTarget[] = [];
  private queue: Pending[] = [];
  private listeners = new Set<() => void>();
  private store: Store | null;

  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      const v = raw ? JSON.parse(raw) as { rows?: unknown; queue?: unknown } : null;
      this.rows = Array.isArray(v?.rows) ? (v!.rows as unknown[]).filter(valid) : [];
      this.queue = Array.isArray(v?.queue) ? v!.queue as Pending[] : [];
    } catch { /* start empty */ }
  }

  private save(): void {
    this.rows = this.rows.slice(0, MAX);
    try { this.store?.setItem(KEY, JSON.stringify({ rows: this.rows, queue: this.queue })); } catch { /* fine */ }
    for (const fn of this.listeners) fn();
  }

  subscribe(fn: () => void): () => void { this.listeners.add(fn); return () => { this.listeners.delete(fn); }; }
  snapshot(): OpTarget[] { return this.rows; }

  /** The newest open operation of this kind for a device (what to deliver). */
  open(device: string, kind: OpKind): OpTarget | null {
    const d = device.toUpperCase();
    return this.rows.find((r) => r.device === d && r.kind === kind && OPEN.includes(r.state)) ?? null;
  }

  async create(kind: OpKind, items: { device: string; payload: OpTarget["payload"] }[], server: OpsServer | null,
               now = Date.now()): Promise<string> {
    const op = newOpId(now);
    const clean = items.map((i) => ({ device: i.device.toUpperCase(), payload: i.payload }));
    if (!clean.length) throw new Error("Hedef cihaz yok.");
    const devs = new Set(clean.map((i) => i.device));
    this.rows = [
      ...clean.map((i) => ({ op, device: i.device, kind, payload: i.payload, state: "waiting" as OpState,
                             createdAt: now, updatedAt: now, via: null, note: null })),
      ...this.rows.map((r) => (devs.has(r.device) && r.kind === kind && OPEN.includes(r.state)
        ? { ...r, state: "superseded" as OpState, updatedAt: now } : r)),
    ];
    this.queue = [...this.queue, { kind: "create", op, opKind: kind, items: clean }];
    this.save();
    if (server) await this.flush(server);
    return op;
  }

  /** What the device (or, on LoRa, the gateway) said about one target. */
  async result(op: string, device: string, state: Exclude<OpState, "waiting" | "superseded">, via: OpVia,
               server: OpsServer | null, note?: string, now = Date.now()): Promise<void> {
    const d = device.toUpperCase();
    this.rows = this.rows.map((r) => (r.op === op && r.device === d ? { ...r, state, via, note: note ?? null, updatedAt: now } : r));
    this.queue = [...this.queue, { kind: "result", op, device: d, state, via, note }];
    this.save();
    if (server) await this.flush(server);
  }

  private async flush(server: OpsServer): Promise<void> {
    while (this.queue.length) {
      const p = this.queue[0];
      try {
        if (p.kind === "create") await server.createOp(p.op, p.opKind, p.items);
        else await server.opResult(p.op, p.device, p.state, p.via, p.note);
      } catch { return; }
      this.queue = this.queue.slice(1);
      this.save();
    }
  }

  async refresh(server: OpsServer): Promise<void> {
    await this.flush(server);
    if (this.queue.length) return;
    this.rows = (await server.deviceOps()).filter(valid);
    this.save();
  }
}

/** Active work stays visible; completed work is collapsed into history.
 * Superseded rows stay in the durable book but do not crowd this view.
 * Old rejected attempts disappear from the active view once replaced.
 */
export function operationSections(rows: OpTarget[]): {
  active: ReturnType<typeof groupOps>; history: ReturnType<typeof groupOps>;
} {
  const groups = groupOps(rows.filter((r) => r.state !== "superseded"));
  const latest = new Map<string, number>();
  for (const group of groups) for (const t of group.targets) {
    const key = t.device + "|" + t.kind;
    latest.set(key, Math.max(latest.get(key) ?? 0, t.createdAt));
  }
  const active: typeof groups = [], history: typeof groups = [];
  for (const group of groups) {
    const visible = group.targets.some((t) => OPEN.includes(t.state) ||
      (t.state === "rejected" && t.createdAt >= (latest.get(t.device + "|" + t.kind) ?? 0)));
    (visible ? active : history).push(group);
  }
  return { active, history };
}

/** Operations grouped for the list: newest first. */
export function groupOps(rows: OpTarget[]): { op: string; kind: OpKind; createdAt: number; targets: OpTarget[] }[] {
  const by = new Map<string, { op: string; kind: OpKind; createdAt: number; targets: OpTarget[] }>();
  for (const r of rows) {
    const g = by.get(r.op) ?? { op: r.op, kind: r.kind, createdAt: r.createdAt, targets: [] };
    g.targets.push(r);
    by.set(r.op, g);
  }
  return [...by.values()].sort((a, b) => b.createdAt - a.createdAt);
}

export function groupLine(g: { targets: OpTarget[] }): string {
  const n = g.targets.length;
  const done = g.targets.filter((t) => t.state === "applied" || t.state === "same").length;
  const open = g.targets.filter((t) => OPEN.includes(t.state)).length;
  const bad = g.targets.filter((t) => t.state === "rejected").length;
  return [`${done}/${n} işlendi`, open ? `${open} sırada` : "", bad ? `${bad} reddedildi` : ""].filter(Boolean).join(" · ");
}

/** What one operation asks of its device, in words: "Rapor aralığı: 30 dk", "Alanlar". */
export function opWhat(t: Pick<OpTarget, "kind" | "payload">): string {
  if (t.kind === "fence") return KIND_WORD.fence;
  const set = (t.payload as SettingsPayload).set ?? {};
  const parts = SETTINGS.filter((d) => d.key in set).map((d) => `${d.title}: ${d.word(set[d.key]!)}`);
  return parts.length ? parts.join(", ") : KIND_WORD.settings;
}
