// Yerel köprü (adım 7): telefonun gateway'den kayıt teslim alan kısmı Android
// yerel koduna taşınıyor (EdgeWire.java, ProductFrame.java). Aynı kural iki
// yerde durduğu için bu test, buradaki başvuru uygulamasından (edgeLink.ts,
// crypto.ts, product.ts) üretilmiş vektörleri bir dosyaya yazar ve dosyanın
// güncel olduğunu denetler; Java tarafı aynı dosyayı okuyup aynı sonucu
// vermek zorundadır (tools/orun_android_mantik_test.sh).
//   Dosyayı yeniden üretmek: ORUN_WRITE_VECTORS=1 npm test
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { ascii, concat, hex } from "../src/core/bytes.ts";
import { aesCcmDecrypt, aesCcmEncrypt, hkdfSha256 } from "../src/core/crypto.ts";
import { acceptMac, decodeGatewayMessage, frames, Reassembler } from "../src/core/edgeLink.ts";
import { openProductFrame, Reject, TEST_CREDENTIAL_ID, TEST_KEY_EPOCH, TEST_ROOT } from "../src/core/product.ts";
import { TEST_EDGE_KEY } from "./edgeKeyFixture.ts";
import { KAT_EVENT, KAT_PERIODIC, seal } from "./fixtures.ts";

const FILE = new URL("../../../tools/android_mantik/bridge_vectors.txt", import.meta.url);

// Deterministic PRNG (mulberry32): the file must not change between runs.
function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function generate(): string {
  const r = rng(20261114);
  const byte = () => Math.floor(r() * 256);
  const bytes = (n: number) => new Uint8Array(n).map(byte);
  const lines: string[] = ["# ORUN yerel köprü vektörleri; test/bridgeVectors.test.ts üretir. Elle değiştirme."];

  // AES-CCM: random sizes, plus single-bit tampering.
  for (let i = 0; i < 40; i++) {
    const key = bytes(16), nonce = bytes(13), aad = bytes(Math.floor(r() * 40)), plain = bytes(Math.floor(r() * 80));
    const sealed = aesCcmEncrypt(key, nonce, plain, aad, 8);
    const bad = sealed.slice(); if (bad.length) bad[Math.floor(r() * bad.length)] ^= 1 << Math.floor(r() * 8);
    for (const s of [sealed, bad]) {
      const out = aesCcmDecrypt(key, nonce, s, aad, 8);
      lines.push(["ccm", hex(key), hex(nonce), hex(aad), hex(s), out ? hex(out) : "NULL"].join("|"));
    }
  }

  // Framing: every size, random content and ids.
  for (let size = 1; size <= 160; size++) {
    const msg = bytes(size), id = byte();
    lines.push(["frames", String(id), hex(msg), frames(msg, id).map(hex).join(",")].join("|"));
  }
  // Reassembly of broken sequences: list of frames -> what comes out ("-" none).
  for (let i = 0; i < 60; i++) {
    const msg = bytes(1 + Math.floor(r() * 120)), list = frames(msg, byte());
    const pick: Uint8Array[] = [];
    for (const f of list) {
      const roll = r();
      if (roll < 0.1) continue;                                   // drop
      const g = f.slice();
      if (roll < 0.15) g[0] = 2;                                  // bad version
      else if (roll < 0.2) g[3] ^= 1;                             // bad index
      pick.push(g);
      if (roll > 0.95) pick.push(g);                              // repeat
    }
    const re = new Reassembler();
    const outs = pick.map((f) => { const o = re.push(f); return o ? hex(o) : "-"; });
    lines.push(["reassemble", pick.map(hex).join(","), outs.join(",")].join("|"));
  }

  // Accept MAC.
  for (let i = 0; i < 20; i++) {
    const gw = BigInt("0x" + hex(bytes(8))), page = Math.floor(r() * 65536), slot = Math.floor(r() * 65536);
    const gen = BigInt("0x" + hex(bytes(8))), object = bytes(100);
    lines.push(["mac", gw.toString(16).toUpperCase().padStart(16, "0"), String(page), String(slot),
      gen.toString(16).toUpperCase().padStart(16, "0"), hex(object), hex(TEST_EDGE_KEY),
      hex(acceptMac(gw, page, slot, gen, object, TEST_EDGE_KEY))].join("|"));
  }

  // Gateway messages: valid shapes and random bytes.
  const msgs: Uint8Array[] = [
    new Uint8Array([1, 9, 0xa4, 0x62, 0xbd, 0x4b, 0x27, 0x5b, 0xa5, 0, 3, 4, 0x20]),
    concat(new Uint8Array([2, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 7, 1, 0xff, 0xa6, 0xfa, 4]), bytes(4)),
    concat(new Uint8Array([6, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 3]), bytes(3)),
    new Uint8Array([3, 0, 9]), new Uint8Array([4, 0, 1, 0, 2, 3]), new Uint8Array([5, 0, 1, 1]),
  ];
  for (let i = 0; i < 60; i++) {
    const m = bytes(1 + Math.floor(r() * 30)); m[0] = 1 + Math.floor(r() * 7); msgs.push(m);
  }
  for (const m of msgs) {
    const d = decodeGatewayMessage(m);
    let text = "NULL";
    if (d) {
      if (d.type === "hello") text = `hello:${d.gateway.toString(16).toUpperCase().padStart(16, "0")}:${d.held}:${d.capacity}`;
      else if (d.type === "held" || d.type === "heldPreview")
        text = `${d.type}:${d.page}:${d.slot}:${d.generation.toString(16).toUpperCase().padStart(16, "0")}:` +
          `${d.link ? `${d.link.rssi}:${d.link.snr}` : "nolink"}:${hex(d.object)}`;
      else if (d.type === "dumpEnd") text = `dumpEnd:${d.count}`;
      else if (d.type === "acceptResult") text = `acceptResult:${d.page}:${d.slot}:${d.code}`;
      else if (d.type === "reclaimed") text = `reclaimed:${d.page}:${d.ok ? 1 : 0}`;
    }
    lines.push(["msg", hex(m), text].join("|"));
  }

  // Product frames: canonical KATs mutated one byte at a time (the rules that
  // decide "noncanonical"), sealed with the bench credential, plus tampering.
  const key = hkdfSha256(TEST_ROOT, TEST_CREDENTIAL_ID,
    concat(ascii("ORUN-TLP-V2-AEAD"), new Uint8Array([1]), new Uint8Array([1, 2, 3, 4])), 16);
  const lookup = () => ({ trafficKey: key });
  const opened = (raw: Uint8Array): string => {
    try {
      const o = openProductFrame(raw, lookup);
      return `OK:${hex(o.deviceId)}:${hex(o.incarnation)}:${o.sequence}:${o.family}:${hex(o.plaintext)}`;
    } catch (e) {
      if (e instanceof Reject) return "REJECT";
      throw e;
    }
  };
  let counter = 1000;
  for (const kat of [KAT_PERIODIC(), KAT_EVENT()]) {
    const base = openProductFrame(kat, lookup).plaintext;
    lines.push(["open", String(TEST_KEY_EPOCH), hex(key), hex(kat), opened(kat)].join("|"));
    for (let i = 0; i < 400; i++) {
      const p = base.slice();
      const n = 1 + Math.floor(r() * 2);
      for (let k = 0; k < n; k++) {
        const at = Math.floor(r() * p.length);
        const roll = r();
        p[at] = roll < 0.4 ? 0 : roll < 0.6 ? 0xff : roll < 0.8 ? p[at] ^ (1 << Math.floor(r() * 8)) : byte();
      }
      const raw = seal(kat, p, counter++);
      lines.push(["open", String(TEST_KEY_EPOCH), hex(key), hex(raw), opened(raw)].join("|"));
    }
    for (let i = 0; i < 30; i++) {
      const raw = kat.slice(); raw[Math.floor(r() * raw.length)] ^= 1 << Math.floor(r() * 8);
      lines.push(["open", String(TEST_KEY_EPOCH), hex(key), hex(raw), opened(raw)].join("|"));
    }
  }
  return lines.join("\n") + "\n";
}

test("yerel köprü vektörleri güncel (Java tarafı aynı sonucu vermek zorunda)", () => {
  const text = generate();
  if (process.env.ORUN_WRITE_VECTORS === "1") writeFileSync(FILE, text);
  const kept = readFileSync(FILE, "utf8");
  assert.equal(kept, text, "tools/android_mantik/bridge_vectors.txt eski: ORUN_WRITE_VECTORS=1 npm test");
  // Both outcomes are really exercised.
  assert.ok(/\|OK:/.test(text) && /\|REJECT$/m.test(text));
});
