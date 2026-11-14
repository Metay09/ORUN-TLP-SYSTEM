// Yerel köprü (adım 7): gateway'den kayıt teslim alma mantığı (EdgeCore)
// Android'de BridgeCore.java olarak yeniden yazıldı. İkisinin her adımda aynı
// şeyi yaptığını göstermek için burada, gerçek EdgeCore ile rastgele ama
// tekrarlanabilir senaryolar oynatılır (bağlanma, kayıtlar, tekrarlar,
// çakışmalar, bozuk kayıtlar, ön izleme, DUMP END, onay/meşgul/red, sayfa
// silme, zaman aşımı, kopma) ve her adımın sonucu bir dosyaya yazılır.
// BridgeCoreCheck.java aynı senaryoları oynatıp aynı sonuçları vermek zorunda.
//   Dosyayı yeniden üretmek: ORUN_WRITE_VECTORS=1 npm test
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { ascii, concat, hex } from "../src/core/bytes.ts";
import { hkdfSha256 } from "../src/core/crypto.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { SCHEMA } from "../src/core/db.ts";
import { openProductFrame, TEST_CREDENTIAL_ID, TEST_KEY_EPOCH, TEST_ROOT } from "../src/core/product.ts";
import { TEST_EDGE_KEY } from "./edgeKeyFixture.ts";
import { KAT_PERIODIC, seal, track } from "./fixtures.ts";
import { NodeDb } from "./nodeDb.ts";

const FILE = new URL("../../../tools/android_mantik/bridge_core_vectors.txt", import.meta.url);
const GW = "09A462BD4B275BA5";

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

const u16 = (n: number) => new Uint8Array([(n >> 8) & 0xff, n & 0xff]);
const u64 = (n: number) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt(n)); return b; };

async function generate(): Promise<string> {
  const r = rng(77);
  const pick = <T,>(a: T[]) => a[Math.floor(r() * a.length)];
  const key = hkdfSha256(TEST_ROOT, TEST_CREDENTIAL_ID,
    concat(ascii("ORUN-TLP-V2-AEAD"), new Uint8Array([1]), new Uint8Array([1, 2, 3, 4])), 16);
  const lookup = () => ({ trafficKey: key });
  // Records: a walking track, the same identities with other content
  // (conflicts), and tampered frames (rejected).
  const good = track(12);
  const kat = KAT_PERIODIC();
  const conflicting = good.slice(0, 4).map((f, i) => {
    const p = openProductFrame(f, lookup).plaintext.slice();
    p[57] ^= 0x01;                                    // battery mV, still canonical
    return seal(kat, p, 9000 + i);
  });
  const broken = good.slice(0, 3).map((f, i) => { const b = f.slice(); b[40 + i] ^= 0x20; return b; });
  const frames = [...good, ...good, ...conflicting, ...broken];

  const lines = ["# ORUN yerel köprü çekirdek senaryoları; test/bridgeCoreVectors.test.ts üretir. Elle değiştirme.",
                 `key|${TEST_KEY_EPOCH}|${hex(key)}|${hex(TEST_EDGE_KEY)}|${GW}`];
  for (let s = 0; s < 60; s++) {
    const withKey = s % 7 !== 6;
    let now = 1_800_000_000;
    const db = new NodeDb();
    for (const q of SCHEMA) await db.execute(q);
    const core = new EdgeCore(db, { credentials: lookup, edgeKey: () => (withKey ? TEST_EDGE_KEY : null), clock: () => now });
    await core.init();
    lines.push(`scenario|${s}|${withKey ? 1 : 0}`);
    let lastAccept: Uint8Array | null = null;
    const steps = 30 + Math.floor(r() * 30);
    for (let i = 0; i < steps; i++) {
      const roll = r();
      let op: string;
      if (i === 0 || roll < 0.05) {
        const held = Math.floor(r() * 20);
        const m = concat(new Uint8Array([1]), Uint8Array.from(GW.match(/../g)!.map((h) => parseInt(h, 16))), u16(held), u16(1056));
        op = `msg|${hex(m)}`;
      } else if (roll < 0.55) {
        const f = pick(frames);
        const page = Math.floor(r() * 3), slot = Math.floor(r() * 6), gen = 1 + Math.floor(r() * 2);
        const link = r() < 0.5;
        const type = r() < 0.12 ? 6 : 2;
        const m = concat(new Uint8Array([type]), u16(page), u16(slot), u64(gen),
          new Uint8Array([link ? 1 : 0]), link ? new Uint8Array([0xff, 0x9f, 0xfd]) : new Uint8Array(3),
          new Uint8Array([f.length]), f);
        op = `msg|${hex(m)}`;
      } else if (roll < 0.75) {
        const code = Math.floor(r() * 9);                 // 8 = unknown code
        let page = Math.floor(r() * 3), slot = Math.floor(r() * 6);
        if (lastAccept && r() < 0.75) { page = (lastAccept[1] << 8) | lastAccept[2]; slot = (lastAccept[3] << 8) | lastAccept[4]; }
        op = `msg|${hex(concat(new Uint8Array([4]), u16(page), u16(slot), new Uint8Array([code])))}`;
      } else if (roll < 0.82) {
        op = `msg|${hex(concat(new Uint8Array([3]), u16(Math.floor(r() * 8))))}`;
      } else if (roll < 0.88) {
        op = `msg|${hex(concat(new Uint8Array([5]), u16(Math.floor(r() * 3)), new Uint8Array([r() < 0.8 ? 1 : 0])))}`;
      } else if (roll < 0.96) {
        op = `tick|${1 + Math.floor(r() * 4)}`;
      } else if (roll < 0.98) {
        op = "end";
      } else {
        op = `msg|${hex(new Uint8Array([Math.floor(r() * 256), 1, 2]))}`;  // junk
      }
      const [kind, arg] = op.split("|");
      if (kind === "msg") await core.handleMessage(Uint8Array.from(arg.match(/../g)!.map((h) => parseInt(h, 16))));
      else if (kind === "tick") now += Number(arg);
      else await core.sessionEnd();
      const accept = core.nextAcceptMessage(now);       // the link pumps after every event
      if (accept) lastAccept = accept;
      const c = core.counts;
      const records = Number((await db.query("SELECT count(*) AS n FROM observations"))[0].n);
      const conflicts = Number((await db.query("SELECT count(*) AS n FROM conflicts"))[0].n);
      const held = core.heldNow();
      lines.push(["step", op.replace("|", ":"), accept ? hex(accept) : "-",
        `${c.STORED},${c.DUPLICATE},${c.REJECTED},${c.CONFLICT},${c.UNCERTAIN}`,
        String(core.handedOff), String(core.acceptRejected), held === null ? "null" : String(held),
        String(core.pendingAccepts()), core.gatewayId === null ? "null" : core.gatewayId.toString(16).toUpperCase().padStart(16, "0"),
        String(records), String(conflicts)].join("|"));
    }
  }
  return lines.join("\n") + "\n";
}

test("yerel köprü çekirdek senaryoları güncel (BridgeCore.java aynı adımları vermek zorunda)", async () => {
  const text = await generate();
  if (process.env.ORUN_WRITE_VECTORS === "1") writeFileSync(FILE, text);
  assert.equal(readFileSync(FILE, "utf8"), text, "bridge_core_vectors.txt eski: ORUN_WRITE_VECTORS=1 npm test");
  // The scenarios really reach every branch.
  for (const want of [/\|[0-9A-F]{90}\|/, /\|[1-9]\d*,\d+,\d+,\d+,0\|/, /,[1-9]\d*,0\|/, /\|\d+,\d+,[1-9]\d*,/])
    assert.match(text, want);
});
