// End-to-end helper for firmware/tests/sf5/test_sf5l8_backend_sync.py:
// a phone ledger filled over BLE messages, uploaded to the REAL Python bench
// backend with fetch (the same call the app makes, minus Capacitor).
// stdin: {"url", "token", "frames": [hex], "links": [[rssi, snr] | null], "t0"}
// stdout: {"phone": points, "counts", "run"}
import { readFileSync } from "node:fs";
import { concat, fromHex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { benchCredentials } from "../src/core/product.ts";
import { Uploader } from "../src/core/sync.ts";
import { fetchPost } from "../src/platform/http.ts";
import { NodeDb } from "./nodeDb.ts";

const input = JSON.parse(readFileSync(0, "utf8"));
let now: number = input.t0;
const db = new NodeDb();
const core = new EdgeCore(db, { credentials: benchCredentials, clock: () => now });
await core.init();
await core.sessionAlive("ble");
await core.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00000420")));
for (let i = 0; i < input.frames.length; i++) {
  const obj = fromHex(input.frames[i]);
  const link = input.links[i] as [number, number] | null;
  const h = new Uint8Array(18); const v = new DataView(h.buffer);
  h[0] = 2; v.setUint16(1, 0); v.setUint16(3, i); v.setBigUint64(5, 1n);
  if (link) { h[13] = 1; v.setInt16(14, link[0]); v.setInt8(16, link[1]); }
  h[17] = obj.length;
  now = input.arrivals[i];
  await core.handleMessage(concat(h, obj));
}
await core.sessionAlive("ble");
const up = new Uploader(db, fetchPost, (fn) => core.exclusive(fn), () => now);
const run = await up.drain(input.url, input.token);
const again = await up.runOnce(input.url, input.token);
process.stdout.write(JSON.stringify({
  phone: (await core.points()).points, counts: await up.counts(), run, again,
}));
