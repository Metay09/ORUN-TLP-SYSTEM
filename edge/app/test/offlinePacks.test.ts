import assert from "node:assert/strict";
import { test } from "node:test";
import {
  checkInstallSpace, chooseInstalledPack, contains, packFileName, parseOfflineCatalog,
  parseOfflinePack, REQUESTED_REGIONS, type OfflinePack,
} from "../src/features/map/offlinePacks/catalog.ts";
import { sha256 } from "@noble/hashes/sha2.js";
import { streamVerifiedPmtiles } from "../src/features/map/offlinePacks/store.ts";

const header = new Uint8Array(127);
header.set(new TextEncoder().encode("PMTiles"));
header[7] = 3;
const toHex = (bytes: Uint8Array) => [...bytes].map((x) => x.toString(16).padStart(2, "0")).join("");
const goodHash = toHex(sha256(header));

const make = (more: Partial<OfflinePack> = {}): OfflinePack => ({
  id: "aydin-roads", version: "2026-10-26", title: "Aydın temel harita",
  kind: "protomaps-v4-vector", bounds: [27, 37, 29, 39],
  minZoom: 0, maxZoom: 15, bytes: header.length, sha256: goodHash,
  url: "https://maps.example.org/aydin-roads.pmtiles",
  attribution: "© OpenStreetMap contributors", license: "ODbL Produced Work",
  ...more,
});

test("offline: region choices do not claim that Aydın or Muğla are pre-downloaded", () => {
  assert.deepEqual(REQUESTED_REGIONS.map((r) => r.id), ["aydin", "mugla"]);
  assert.equal(REQUESTED_REGIONS.some((r) => "url" in r), false);
});

test("offline: catalog validates prebuilt immutable HTTPS PMTiles archives", () => {
  const p = parseOfflinePack(make());
  assert.equal(p.maxZoom, 15);
  assert.equal(packFileName(p), `aydin-roads-2026-10-26-${goodHash.slice(0,16)}.pmtiles`);
  assert.equal(parseOfflineCatalog({ schema: "orun-offline-packs-v1", packs: [p] }).length, 1);
});

test("offline: untrusted sources and path traversals fail closed", () => {
  for (const url of [
    "http://tile.openstreetmap.org/test.pmtiles",
    "file:///storage/maps/secret.pmtiles",
    "javascript:alert(1)",
    "https://user:pass@maps.example.org/a.pmtiles",
    "https://maps.example.org/test.zip",
  ]) {
    assert.throws(() => parseOfflinePack(make({ url })), /HTTPS|adres/);
  }
  assert.throws(() => parseOfflinePack(make({ id: "../attack" })), /kimliği/);
  assert.throws(() => parseOfflinePack(make({ sha256: "1234" })), /SHA-256/);
  assert.throws(() => parseOfflinePack(make({ kind: "bad" as OfflinePack["kind"] })), /türü/);
});

test("offline: reject invalid bounds, misleading zoom claims, duplicate packages", () => {
  assert.throws(() => parseOfflinePack(make({ bounds: [29, 38, 27, 37] })), /koordinat/);
  assert.throws(() => parseOfflinePack(make({ maxZoom: 20 })), /z15/);
  assert.throws(() => parseOfflineCatalog({ schema: "orun-offline-packs-v1", packs: [make(), make()] }), /tekrarlanan/);
  assert.throws(() => parseOfflineCatalog({ packs: [make()] }), /kataloğu/);
});

test("offline: quota preflight keeps room for phone observation ledger", () => {
  assert.equal(checkInstallSpace(make(), { quota: 100_000_000, usage: 0 }), null);
  assert.match(checkInstallSpace(make(), { quota: 1_000, usage: 990 })!, /boş alan/);
  assert.match(checkInstallSpace(make(), null)!, /ölçülemedi/);
});

test("offline: choose by geographic bounds and detailed source zoom without changing GPS", () => {
  const broad = { pack: make({ id: "mugla-roads", bounds: [26, 36, 30, 40], maxZoom: 14 }), fileName: "a", installedAt: 1 };
  const detailed = { pack: make({ id: "aydin-roads" }), fileName: "b", installedAt: 2 };
  assert.equal(contains(detailed.pack, 28, 38), true);
  assert.equal(contains(detailed.pack, 40, 38), false);
  assert.equal(chooseInstalledPack([broad, detailed], 28, 38)?.fileName, "b");
  assert.equal(chooseInstalledPack([broad, detailed], -30, 38), null);
});

test("offline: chunked streaming verifies exact PMTiles magic, byte count and sha256", async () => {
  const chunks = [header.subarray(0, 3), header.subarray(3, 50), header.subarray(50)];
  const received: Uint8Array[] = [];
  const events: number[] = [];
  const input = new ReadableStream<Uint8Array>({
    start(controller) { for (const c of chunks) controller.enqueue(c); controller.close(); },
  });
  const writer = { write: async (data: FileSystemWriteChunkType) => {
    if (!(data instanceof Uint8Array)) throw new Error("expected bytes");
    received.push(data);
  }};
  await streamVerifiedPmtiles(input, writer, make(), undefined, (p) => events.push(p.receivedBytes));
  assert.equal(received.reduce((n, p) => n + p.byteLength, 0), header.length);
  assert.deepEqual(events, [3, 50, 127]);
});

test("offline: corrupt bytes, truncated streams and fake files never pass verification", async () => {
  async function attempt(bytes: Uint8Array, expected: OfflinePack) {
    return streamVerifiedPmtiles(new ReadableStream<Uint8Array>({
      start(controller) { controller.enqueue(bytes); controller.close(); },
    }), { write: async () => undefined }, expected);
  }
  const changed = header.slice();
  changed[13] = 1;
  await assert.rejects(() => attempt(changed, make()), /SHA-256/);
  await assert.rejects(() => attempt(header.slice(0,126), make()), /eksik/);
  const fake = header.slice();
  fake[0] = 0;
  await assert.rejects(() => attempt(fake, make({ sha256: toHex(sha256(fake)) })), /PMTiles/);
});
