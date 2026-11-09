import assert from "node:assert/strict";
import { test } from "node:test";
import { createHash } from "node:crypto";
import { deflateRawSync } from "node:zlib";
import { inspectFirmwarePackage } from "../src/app/firmwarePackage.ts";

const commit = "0123456789abcdef0123456789abcdef01234567";
const enc = new TextEncoder();
const hash = (b: Uint8Array) => createHash("sha256").update(b).digest("hex");
const crc32 = (b: Uint8Array): number => {
  let c = 0xffffffff;
  for (const n of b) { c ^= n; for (let i = 0; i < 8; i++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1; }
  return (c ^ 0xffffffff) >>> 0;
};

function zip(files: { name: string; body: Uint8Array; lieAboutSize?: number }[], deflate = true): Uint8Array {
  const local: Buffer[] = [], cd: Buffer[] = [];
  let offset = 0;
  for (const f of files) {
    const name = Buffer.from(f.name), raw = Buffer.from(f.body), comp = deflate ? deflateRawSync(raw) : raw;
    const len = f.lieAboutSize ?? raw.length, crc = crc32(raw);
    const h = Buffer.alloc(30);
    h.writeUInt32LE(0x04034b50); h.writeUInt16LE(20, 4); h.writeUInt16LE(deflate ? 8 : 0, 8);
    h.writeUInt32LE(crc, 14); h.writeUInt32LE(comp.length, 18); h.writeUInt32LE(len, 22);
    h.writeUInt16LE(name.length, 26);
    local.push(h, name, comp);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50); c.writeUInt16LE(20, 4); c.writeUInt16LE(20, 6);
    c.writeUInt16LE(deflate ? 8 : 0, 10); c.writeUInt32LE(crc, 16);
    c.writeUInt32LE(comp.length, 20); c.writeUInt32LE(len, 24);
    c.writeUInt16LE(name.length, 28); c.writeUInt32LE(offset, 42);
    cd.push(c, name);
    offset += h.length + name.length + comp.length;
  }
  const dir = Buffer.concat(cd);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10); end.writeUInt32LE(dir.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...local, dir, end]));
}

function fixture(options: { board?: string; hash?: string; extra?: boolean; deflate?: boolean } = {}): Uint8Array {
  const bin = enc.encode("Firmware image bytes"), dat = enc.encode("Nordic init packet");
  const m = {
    format: "orun-serial-dfu", version: 1, source_commit: commit,
    target: { board: options.board ?? "RAK4630", chip: "nRF52840", env: "rak4630_orun" },
    application: {
      bin: "application.bin", bin_sha256: options.hash ?? hash(bin), bin_size: bin.length,
      dat: "application.dat", dat_sha256: hash(dat), dat_size: dat.length,
    },
  };
  return zip([
    { name: "orun-package.json", body: enc.encode(JSON.stringify(m)) },
    { name: "application.bin", body: bin }, { name: "application.dat", body: dat },
    ...(options.extra ? [{ name: "bootloader.bin", body: enc.encode("bad") }] : []),
  ], options.deflate !== false);
}

test("reads real deflate ORUN application-only package and verifies hashes", async () => {
  const p = await inspectFirmwarePackage(fixture());
  assert.equal(p.target, "RAK4630");
  assert.equal(p.sourceCommit, commit);
  assert.equal(p.imageSha256, hash(p.image));
  assert.equal(p.initSha256, hash(p.initPacket));
});

test("also accepts stored ZIP and never trusts just the file extension", async () => {
  const p = await inspectFirmwarePackage(fixture({ deflate: false }));
  assert.equal(p.image.length, "Firmware image bytes".length);
  await assert.rejects(inspectFirmwarePackage(enc.encode("fake.zip")), /geçersiz/);
});

test("refuses altered image hash and wrong device family", async () => {
  await assert.rejects(inspectFirmwarePackage(fixture({ hash: "0".repeat(64) })), /SHA-256/);
  await assert.rejects(inspectFirmwarePackage(fixture({ board: "RAK9999" })), /Hedef/);
});

test("refuses extra bootloader file", async () => {
  await assert.rejects(inspectFirmwarePackage(fixture({ extra: true })), /dizini|dosya sayısı/);
});

test("limits decompression even if ZIP lies about uncompressed size", async () => {
  const meta = enc.encode(JSON.stringify({ format: "orun-serial-dfu" }));
  const bomb = zip([
    { name: "orun-package.json", body: meta },
    { name: "application.bin", body: enc.encode("x".repeat(100_000)), lieAboutSize: 12 },
    { name: "application.dat", body: enc.encode("init") },
  ]);
  await assert.rejects(inspectFirmwarePackage(bomb), /geçersiz/);
});
