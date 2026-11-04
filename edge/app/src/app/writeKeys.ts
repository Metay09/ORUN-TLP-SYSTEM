// 4c-1b: hesabın tasmaları için Bluetooth yazma anahtarları. Sunucudan gelir
// (her tasma ve anahtar dönemi için; tasmanın kök anahtarı değil), telefonda
// saklanır ki sahada internetsiz de yazılabilsin; hesaptan çıkınca silinir.
// Sunucu listesi esastır: tasma yeniden eklenince eski dönemin anahtarı gider.
import { fromHex } from "../core/bytes.ts";

export interface WriteKey { device: string; epoch: number; writeKey: string; }
export interface WriteKeyServer { writeKeys(): Promise<WriteKey[]>; }

type Store = Pick<Storage, "getItem" | "setItem" | "removeItem">;
const KEY = "orun.writeKeys.v1";
const ID16 = /^[0-9A-F]{16}$/;
const HEX32 = /^[0-9A-F]{64}$/;

const id = (device: string, epoch: number) => `${device.toUpperCase()}:${epoch}`;

export function validWriteKeys(list: unknown): WriteKey[] {
  if (!Array.isArray(list)) return [];
  return list.flatMap((k) => {
    const r = k as Record<string, unknown>;
    const device = String(r?.device ?? "").toUpperCase();
    const epoch = Number(r?.epoch);
    const writeKey = String(r?.write_key ?? r?.writeKey ?? "").toUpperCase();
    return ID16.test(device) && Number.isInteger(epoch) && epoch > 0 && epoch < 0xffffffff && HEX32.test(writeKey)
      ? [{ device, epoch, writeKey }] : [];
  });
}

export class WriteKeyBook {
  private keys: Record<string, string> = {};
  private store: Store | null;

  constructor(store: Store | null) {
    this.store = store;
    try {
      const raw = store?.getItem(KEY);
      for (const k of validWriteKeys(raw ? JSON.parse(raw) : null)) this.keys[id(k.device, k.epoch)] = k.writeKey;
    } catch { /* start empty */ }
  }

  private save(): void {
    const list = Object.entries(this.keys).map(([k, writeKey]) => {
      const [device, epoch] = k.split(":");
      return { device, epoch: Number(epoch), write_key: writeKey };
    });
    try { this.store?.setItem(KEY, JSON.stringify(list)); } catch { /* memory only */ }
  }

  get size(): number { return Object.keys(this.keys).length; }

  /** The key for exactly this tasma and epoch (what its challenge names). */
  readonly get = (device: string, epoch: number): Uint8Array | null => {
    const k = this.keys[id(device, epoch)];
    return k ? fromHex(k) : null;
  };

  has(device: string): boolean {
    const d = device.toUpperCase() + ":";
    return Object.keys(this.keys).some((k) => k.startsWith(d));
  }

  /** Takes the server's list as it is; returns how many keys changed. */
  async refresh(server: WriteKeyServer): Promise<number> {
    const next: Record<string, string> = {};
    for (const k of validWriteKeys((await server.writeKeys()).map((x) => ({ device: x.device, epoch: x.epoch, write_key: x.writeKey }))))
      next[id(k.device, k.epoch)] = k.writeKey;
    const all = new Set([...Object.keys(next), ...Object.keys(this.keys)]);
    let changed = 0;
    for (const k of all) if (next[k] !== this.keys[k]) changed++;
    this.keys = next;
    this.save();
    return changed;
  }

  /** Sign-out: no write authority stays on the phone. */
  clear(): void {
    this.keys = {};
    try { this.store?.removeItem(KEY); } catch { /* fine */ }
  }
}
