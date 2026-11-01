// 2: internetsiz harita — sunucudaki paket listesi, sürüm teklifi, çevrimdışı stil.
import assert from "node:assert/strict";
import { test } from "node:test";
import { OwnerAccountClient } from "../src/app/ownerAccountClient.ts";
import {
  packDate, packOffers, packSize, type InstalledPack, type OfflinePack,
} from "../src/features/map/offlinePacks/catalog.ts";
import { OFFLINE_SOURCE, offlineStyle } from "../src/features/map/offlinePacks/offlineStyle.ts";

const ORIGIN = "https://orun.example";
const TOKEN = "T".repeat(64);
const now = () => Math.floor(Date.now() / 1000);
const identity = { site: "orun-main", actor: "owner", username: "ownerlogin",
                   installation: "account-" + "a".repeat(64), expiresAt: now() + 86400 };

const pack = (version: string, sha = version.padEnd(64, "0").replace(/[^0-9a-f]/g, "0")): OfflinePack => ({
  id: "aydin-mugla", version, title: "Aydın–Muğla harita", kind: "protomaps-v4-vector",
  bounds: [27, 36.3, 29.9, 38.3], minZoom: 0, maxZoom: 15, bytes: 190 * 1024 * 1024,
  sha256: sha, url: `${ORIGIN}/api/v1/maps/file/aydin-mugla-${version}.pmtiles`,
  attribution: "© OpenStreetMap katkıcıları · Protomaps", license: "ODbL Produced Work",
});
const installed = (p: OfflinePack): InstalledPack => ({ pack: p, installedAt: 1, fileName: "x.pmtiles" });

function client(answer: (url: string, init: RequestInit) => Response): OwnerAccountClient {
  const c = new OwnerAccountClient(ORIGIN, (async (url: string, init: RequestInit) => answer(url, init)) as typeof fetch);
  c.adopt(identity, TOKEN);
  return c;
}
const json = (status: number, body: unknown) =>
  new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

test("sunucunun harita listesi hesap oturumuyla alınır, adresler o sunucuya çözülür", async () => {
  const raw = { ...pack("20261031"), url: "/api/v1/maps/file/aydin-mugla-20261031.pmtiles" };
  let auth = "";
  const c = client((url, init) => {
    assert.equal(url, ORIGIN + "/api/v1/maps/catalog");
    auth = String((init.headers as Record<string, string>).Authorization);
    return json(200, { schema: "orun-offline-packs-v1", packs: [raw] });
  });
  const [p] = await c.mapCatalog();
  assert.equal(auth, "Bearer " + TOKEN);
  assert.equal(p.url, ORIGIN + "/api/v1/maps/file/aydin-mugla-20261031.pmtiles");
  assert.deepEqual(c.mapDownloadHeaders(), { Authorization: "Bearer " + TOKEN });
});

test("başka bir adrese yönlendiren paket kabul edilmez; oturumsuz liste istenmez", async () => {
  for (const url of ["https://evil.example/x.pmtiles", "/api/v1/maps/file/../x.pmtiles", "/other/x.pmtiles"]) {
    const c = client(() => json(200, { schema: "orun-offline-packs-v1", packs: [{ ...pack("20261031"), url }] }));
    await assert.rejects(c.mapCatalog(), /bu sunucuya ait değil/);
  }
  await assert.rejects(client(() => json(401, {})).mapCatalog(), /yeniden giriş/);
  const signedOut = new OwnerAccountClient(ORIGIN, (async () => json(200, {})) as typeof fetch);
  await assert.rejects(signedOut.mapCatalog(), /giriş/);
  assert.throws(() => signedOut.mapDownloadHeaders());
});

test("teklif: telefonda yoksa indir, eski sürüm varsa yenisi, aynısı varsa hiçbir şey", () => {
  const a = pack("20261024", "a".repeat(64)), b = pack("20261031", "b".repeat(64));
  assert.deepEqual(packOffers([], [a, b]).map((o) => [o.pack.version, o.installed]), [["20261031", null]]);
  const old = installed(a);
  assert.deepEqual(packOffers([old], [b]).map((o) => [o.pack.version, o.installed?.pack.version]), [["20261031", "20261024"]]);
  assert.deepEqual(packOffers([installed(b)], [a, b]), []);
  assert.equal(packDate("20261031"), "31.10.2026");
  assert.equal(packDate("2026-10-26"), "2026-10-26");
  assert.equal(packSize(190 * 1024 * 1024), "190 MB");
});

test("çevrimdışı stil: paket kaynağı, uygulama içindeki yazı tipi ve simgeler, Türkçe adlar", () => {
  const style = offlineStyle({ url: "pmtiles://aydin-mugla-20261031-abc.pmtiles", attribution: "© OSM",
                               bounds: [27, 36.3, 29.9, 38.3], maxZoom: 15, title: "x" }, "https://localhost");
  assert.equal(style.glyphs, "https://localhost/map-assets/fonts/{fontstack}/{range}.pbf");
  assert.equal(style.sprite, "https://localhost/map-assets/sprites/light");
  assert.deepEqual(Object.keys(style.sources), [OFFLINE_SOURCE]);
  assert.equal((style.sources[OFFLINE_SOURCE] as { url: string }).url, "pmtiles://aydin-mugla-20261031-abc.pmtiles");
  const text = JSON.stringify(style.layers);
  assert.ok(text.includes('"name:tr"'), "Türkçe ad önce");
  assert.ok(!/https?:\/\//.test(text), "katmanlar internetten bir şey istemez");
  const fonts = new Set(style.layers.flatMap((l) => {
    const f = (l as { layout?: Record<string, unknown> }).layout?.["text-font"];
    return JSON.stringify(f ?? []).match(/Noto Sans [A-Za-z]+/g) ?? [];
  }));
  assert.deepEqual([...fonts].sort(), ["Noto Sans Italic", "Noto Sans Medium", "Noto Sans Regular"]);
});
