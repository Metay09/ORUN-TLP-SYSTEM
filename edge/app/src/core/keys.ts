// Step 3A / owner decision D1: the phone opens records with the read keys
// the backend gave it (per Tracker, per epoch). The public bench test key
// stays only for old bench records.
import { fromHex, hex } from "./bytes.ts";
import type { Db } from "./db.ts";
import { benchCredentials, type CredentialLookup } from "./product.ts";

export interface ReadKey { device: string; epoch: number; trafficKey: string; }

export class KeyRing {
  private keys = new Map<string, Uint8Array>();
  private bench: boolean;

  constructor(bench = true) { this.bench = bench; }

  private static id(device: string, epoch: number): string {
    return `${device.toUpperCase()}:${epoch}`;
  }

  get size(): number { return this.keys.size; }

  /** The lookup the ledger uses; sees later additions. */
  readonly lookup: CredentialLookup = (device, epoch) => {
    const k = this.keys.get(KeyRing.id(hex(device), epoch));
    if (k) return { trafficKey: k };
    return this.bench ? benchCredentials(device, epoch) : null;
  };

  async load(db: Db): Promise<void> {
    for (const r of await db.query("SELECT device_id,key_epoch,traffic_key FROM read_keys"))
      this.keys.set(KeyRing.id(String(r.device_id), Number(r.key_epoch)), fromHex(String(r.traffic_key)));
  }

  /** Validates and stores keys; returns how many were new or changed. */
  async store(db: Db, keys: ReadKey[]): Promise<number> {
    const good = keys.filter((k) =>
      /^[0-9A-Fa-f]{16}$/.test(k.device) && Number.isInteger(k.epoch) && k.epoch > 0 &&
      k.epoch < 0xffffffff && /^[0-9A-Fa-f]{32}$/.test(k.trafficKey));
    let changed = 0;
    await db.transaction(async () => {
      for (const k of good) {
        const id = KeyRing.id(k.device, k.epoch);
        const old = this.keys.get(id);
        if (old && hex(old) === k.trafficKey.toUpperCase()) continue;
        await db.run("INSERT OR REPLACE INTO read_keys (device_id,key_epoch,traffic_key) VALUES (?,?,?)",
                     [k.device.toUpperCase(), k.epoch, k.trafficKey.toUpperCase()]);
        changed++;
      }
    });
    for (const k of good) this.keys.set(KeyRing.id(k.device, k.epoch), fromHex(k.trafficKey));
    return changed;
  }
}
