// Y2b: Gateway Edge ACCEPT authority, separate from Tracker read/write keys.
import { fromHex } from "../core/bytes.ts";
export interface EdgeKey { gateway: string; key: string; }
export interface EdgeKeyVault {
  load(): Promise<EdgeKey[]>;
  save(keys: EdgeKey[]): Promise<void>;
  clear(): Promise<void>;
}
export function validEdgeKeys(value: unknown): EdgeKey[] {
  if (!Array.isArray(value) || value.length > 16) throw new Error("Geçersiz teslim anahtarı listesi.");
  const seen = new Set<string>();
  return value.map((k) => {
    if (!k || typeof k.gateway !== "string" || !/^[0-9A-F]{16}$/.test(k.gateway) ||
        typeof k.key !== "string" || !/^[0-9A-F]{64}$/.test(k.key) ||
        (/^0{64}$/.test(k.key) || /^F{64}$/.test(k.key)) || seen.has(k.gateway))
      throw new Error("Geçersiz teslim anahtarı.");
    seen.add(k.gateway);
    return { gateway: k.gateway, key: k.key };
  });
}
export class EdgeKeyBook {
  private keys = new Map<string, Uint8Array>();
  #revision = 0;
  #vault: EdgeKeyVault;
  constructor(vault: EdgeKeyVault) { this.#vault = vault; }
  readonly get = (gateway: string): Uint8Array | null => this.keys.get(gateway) ?? null;
  private replace(keys: EdgeKey[]): void {
    for (const k of this.keys.values()) k.fill(0);
    this.keys = new Map(keys.map(k => [k.gateway, fromHex(k.key)]));
  }
  async load(): Promise<void> {
    const revision = this.#revision;
    const keys = validEdgeKeys(await this.#vault.load());
    if (revision === this.#revision) this.replace(keys);
  }
  async refresh(server: { edgeKeys(): Promise<EdgeKey[]> }): Promise<void> {
    const revision = this.#revision;
    const keys = validEdgeKeys(await server.edgeKeys());
    if (revision !== this.#revision) return;
    await this.#vault.save(keys);
    if (revision === this.#revision) this.replace(keys);
  }
  clear(): void {
    this.#revision++;
    this.replace([]);
    void this.#vault.clear().catch(() => undefined);
  }
}
