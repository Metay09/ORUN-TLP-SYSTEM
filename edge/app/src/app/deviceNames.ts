// 2d: cihaz adları hesapta (sahip kararı). Tasmanın adı hayvanın adıdır,
// gateway'in adı kendi adıdır. Adsız cihaz Bluetooth adıyla görünür:
// ORUN-XXXXXXXX (kimliğin son 8 hanesi). Telefonda bir kopya tutulur ki
// internetsiz açılışta da adlar görünsün; gönderilemeyen ad sonra gönderilir.

export function bleName(id: string): string {
  return "ORUN-" + id.slice(-8).toUpperCase();
}

export function cleanName(name: string): string {
  // eslint-disable-next-line no-control-regex
  return name.replace(/[\u0000-\u001f\u007f-\u009f]/g, " ").split(/\s+/).filter(Boolean).join(" ").slice(0, 40);
}

export interface NameServer {
  deviceNames(): Promise<Record<string, string>>;
  setDeviceName(device: string, name: string): Promise<void>;
}

type Store = Pick<Storage, "getItem" | "setItem">;
const KEY = "orun.deviceNames.v1";
const ID16 = /^[0-9A-F]{16}$/;

export class DeviceNameBook {
  private names: Record<string, string> = {};
  private pending: Record<string, string> = {};
  private listeners = new Set<() => void>();
  private store: Store | null;

  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      const v = raw ? JSON.parse(raw) as { names?: unknown; pending?: unknown } : null;
      this.names = DeviceNameBook.valid(v?.names);
      this.pending = DeviceNameBook.valid(v?.pending, true);
    } catch { /* start empty */ }
  }

  private static valid(v: unknown, allowEmpty = false): Record<string, string> {
    const out: Record<string, string> = {};
    if (!v || typeof v !== "object") return out;
    for (const [k, n] of Object.entries(v as Record<string, unknown>))
      if (ID16.test(k) && typeof n === "string" && (allowEmpty || n) && n.length <= 40) out[k] = n;
    return out;
  }

  private save(): void {
    try { this.store?.setItem(KEY, JSON.stringify({ names: this.names, pending: this.pending })); } catch { /* fine */ }
    for (const fn of this.listeners) fn();
  }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  }

  /** All names (for the herd and lists). */
  snapshot(): Record<string, string> { return this.names; }

  name(id: string): string | null { return this.names[id.toUpperCase()] ?? null; }

  label(id: string): string { return this.name(id) ?? bleName(id); }

  /** Shows at once; sends now, or later if there is no server/internet. */
  async set(id: string, name: string, server: NameServer | null): Promise<void> {
    const dev = id.toUpperCase();
    if (!ID16.test(dev)) throw new Error("Cihaz kimliği geçersiz.");
    const text = cleanName(name);
    const next = { ...this.names };
    if (text) next[dev] = text; else delete next[dev];
    this.names = next;
    this.pending = { ...this.pending, [dev]: text };
    this.save();
    if (server) await this.push(server, dev);
  }

  private async push(server: NameServer, dev: string): Promise<void> {
    const text = this.pending[dev];
    if (text === undefined) return;
    try {
      await server.setDeviceName(dev, text);
      if (this.pending[dev] === text) {
        const rest = { ...this.pending };
        delete rest[dev];
        this.pending = rest;
        this.save();
      }
    } catch { /* stays pending */ }
  }

  /** Send what is waiting, then take the account's names. */
  async refresh(server: NameServer): Promise<void> {
    for (const dev of Object.keys(this.pending)) await this.push(server, dev);
    const remote = DeviceNameBook.valid(await server.deviceNames());
    for (const [dev, text] of Object.entries(this.pending)) {
      if (text) remote[dev] = text; else delete remote[dev];
    }
    this.names = remote;
    this.save();
  }
}
