// Test frames: the shared SF5B vectors and a sealer mirroring
// firmware/tests/sf5/test_sf5l3_edge_map.py seal()/track().
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { ascii, concat, fromHex } from "../src/core/bytes.ts";
import { aesCcmEncrypt, hkdfSha256 } from "../src/core/crypto.ts";
import { benchCredentials, openProductFrame, TEST_CREDENTIAL_ID, TEST_KEY_EPOCH, TEST_ROOT } from "../src/core/product.ts";

const VECTORS = fileURLToPath(new URL("../../../firmware/tests/sf5/sf5b_product_vectors.h", import.meta.url));

export function vector(name: string): Uint8Array {
  const text = readFileSync(VECTORS, "utf8");
  const m = new RegExp(`static const char ${name}\\[\\]\\s*=\\s*((?:\\s*"[0-9a-f]+"\\s*)+);`).exec(text);
  if (!m) throw new Error(name);
  return fromHex([...m[1].matchAll(/"([0-9a-f]+)"/g)].map((x) => x[1]).join(""));
}

export function seal(template: Uint8Array, plaintext: Uint8Array, counter: number): Uint8Array {
  const header = template.slice(0, 28);
  const c = new Uint8Array(8);
  new DataView(c.buffer).setBigUint64(0, BigInt(counter));
  header.set(c, 20);
  const epoch = new Uint8Array(4);
  new DataView(epoch.buffer).setUint32(0, TEST_KEY_EPOCH);
  const key = hkdfSha256(TEST_ROOT, TEST_CREDENTIAL_ID,
                         concat(ascii("ORUN-TLP-V2-AEAD"), new Uint8Array([1]), epoch), 16);
  const nonce = concat(epoch, new Uint8Array([1]), c);
  return concat(header, aesCcmEncrypt(key, nonce, plaintext, header, 8));
}

export const KAT_PERIODIC = () => vector("kKatPeriodicFrameHex");
export const KAT_EVENT = () => vector("kKatEventFrameHex");

/** n canonical PERIODIC frames walking a short path near Çine (as l3.track). */
export function track(n: number): Uint8Array[] {
  const kat = KAT_PERIODIC();
  const base = openProductFrame(kat, benchCredentials).plaintext;
  const dv = new DataView(base.buffer, base.byteOffset, base.byteLength);
  const out: Uint8Array[] = [];
  for (let i = 0; i < n; i++) {
    const p = base.slice();
    const v = new DataView(p.buffer);
    v.setUint32(12, 1000 + i);
    v.setUint32(16, dv.getUint32(16) + 60 * i);
    v.setInt32(24, 376105000 + 40 * i + (i % 2 ? 15 : -15) * 3);
    v.setInt32(28, 280546000 + 55 * i);
    v.setUint16(56, 4100 - i);
    out.push(seal(kat, p, 500 + i));
  }
  return out;
}
