// ORUN KQ-02B: tracker signing authority stays in RAM and a native encrypted
// AndroidKeyStore vault. Never persist write keys in WebView localStorage.
import { fromHex } from "../core/bytes.ts";

export interface WriteKey { device: string; epoch: number; writeKey: string; }
export interface WriteKeyServer { writeKeys(): Promise<WriteKey[]>; }
export interface WriteKeyVault {
  load(): Promise<WriteKey[]>;
  save(keys: WriteKey[]): Promise<void>;
  clear(): Promise<void>;
}

const ID16 = /^[0-9A-F]{16}$/;
const HEX32 = /^[0-9A-F]{64}$/;
const MAX_KEYS = 1024;
const id = (device: string, epoch: number) => `${device.toUpperCase()}:${epoch}`;

export function validWriteKeys(list: unknown): WriteKey[] {
  if (!Array.isArray(list)) return [];
  if (list.length > MAX_KEYS) throw new Error("Çok fazla tasma yazma anahtarı.");
  const seen = new Set<string>();
  return list.flatMap((entry) => {
    if (!entry || typeof entry !== "object" || Array.isArray(entry)) return [];
    const r = entry as Record<string, unknown>;
    const device = String(r.device ?? "").toUpperCase();
    const epoch = Number(r.epoch);
    const writeKey = String(r.write_key ?? r.writeKey ?? "").toUpperCase();
    const ident = id(device, epoch);
    if (!ID16.test(device) || !Number.isInteger(epoch) || epoch <= 0 || epoch >= 0xffffffff ||
        !HEX32.test(writeKey) || /^0{64}$/.test(writeKey) || /^F{64}$/.test(writeKey) ||
        seen.has(ident)) return [];
    seen.add(ident);
    return [{ device, epoch, writeKey }];
  });
}

export class WriteKeyBook {
  private keys: Record<string, string> = {};
  private readonly vault: WriteKeyVault;
  private revision = 0;
  private request = 0;
  private pending: Promise<void> = Promise.resolve();

  constructor(vault: WriteKeyVault) { this.vault = vault; }

  /** Serializes native reads/writes/clears across account switches. */
  private serialize<T>(task: () => Promise<T>): Promise<T> {
    const run = this.pending.then(task, task);
    this.pending = run.then(() => undefined, () => undefined);
    return run;
  }

  private replace(keys: WriteKey[]): void {
    const next: Record<string, string> = {};
    for (const k of keys) next[id(k.device, k.epoch)] = k.writeKey;
    this.keys = next;
  }

  /** On account restore, recover only account-scoped encrypted native keys. */
  async load(): Promise<void> {
    const revision = this.revision;
    await this.serialize(async () => {
      if (revision !== this.revision) return;
      const stored = validWriteKeys(await this.vault.load());
      if (revision === this.revision) this.replace(stored);
    });
  }

  get size(): number { return Object.keys(this.keys).length; }

  readonly get = (device: string, epoch: number): Uint8Array | null => {
    const k = this.keys[id(device, epoch)];
    return k ? fromHex(k) : null;
  };

  active(device: string): { epoch: number; key: Uint8Array } | null {
    const d = device.toUpperCase() + ":";
    let best: { epoch: number; key: Uint8Array } | null = null;
    for (const [k, v] of Object.entries(this.keys)) {
      if (!k.startsWith(d)) continue;
      const epoch = Number(k.slice(d.length));
      if (!best || epoch > best.epoch) best = { epoch, key: fromHex(v) };
    }
    return best;
  }

  has(device: string): boolean {
    const d = device.toUpperCase() + ":";
    return Object.keys(this.keys).some((k) => k.startsWith(d));
  }

  async refresh(server: WriteKeyServer): Promise<number> {
    const revision = this.revision;
    const request = ++this.request;
    const next = validWriteKeys((await server.writeKeys()).map((k) => ({
      device: k.device, epoch: k.epoch, writeKey: k.writeKey,
    })));
    if (revision !== this.revision || request !== this.request) return 0;
    return this.serialize(async () => {
      if (revision !== this.revision || request !== this.request) return 0;
      const before = this.keys;
      const after: Record<string, string> = {};
      for (const k of next) after[id(k.device, k.epoch)] = k.writeKey;
      const all = new Set([...Object.keys(after), ...Object.keys(before)]);
      let changed = 0;
      for (const key of all) if (after[key] !== before[key]) changed++;
      // Fail closed: if encrypted persistence fails, do not activate new authority.
      await this.vault.save(next);
      if (revision === this.revision && request === this.request) this.replace(next);
      return revision === this.revision && request === this.request ? changed : 0;
    });
  }

  /** RAM authority disappears synchronously; the returned promise confirms
   * durable native deletion before another account can sign in. */
  clear(): Promise<void> {
    this.revision++;
    this.request++;
    this.keys = {};
    return this.serialize(() => this.vault.clear());
  }
}
