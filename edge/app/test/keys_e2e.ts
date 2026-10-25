// Helper for firmware/tests/sf5/test_step3a_credentials.py: the phone gets
// its read keys from the REAL backend (GET /v1/edge/keys), opens Tracker
// records sealed with a real credential, and uploads them.
// stdin: {"url", "token", "frames": [hex]}  stdout: {"keys", "states", "run"}
import { readFileSync } from "node:fs";
import { concat, fromHex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { KeyRing } from "../src/core/keys.ts";
import { fetchReadKeys, Uploader } from "../src/core/sync.ts";
import { fetchGet, fetchPost } from "../src/platform/http.ts";
import { NodeDb } from "./nodeDb.ts";

const input = JSON.parse(readFileSync(0, "utf8"));
const db = new NodeDb();
const ring = new KeyRing(false);           // no bench key at all
const core = new EdgeCore(db, { credentials: ring.lookup, clock: () => 1_800_000_000 });
await core.init();
const before = await core.handleMessage(concat(new Uint8Array([1]), fromHex("09A462BD4B275BA5"), fromHex("00010420")));
const held = (i: number, obj: Uint8Array) => {
  const h = new Uint8Array(18); const v = new DataView(h.buffer);
  h[0] = 2; v.setUint16(3, i); v.setBigUint64(5, 1n); h[17] = obj.length;
  return concat(h, obj);
};
const states: string[] = [];
const first = (await core.handleMessage(held(0, fromHex(input.frames[0])))) ?? "";
states.push(first.split(" ")[0]);
const keys = await fetchReadKeys(fetchGet, input.url, input.token);
await core.exclusive(() => ring.store(db, keys));
for (let i = 0; i < input.frames.length; i++)
  states.push(((await core.handleMessage(held(i, fromHex(input.frames[i])))) ?? "").split(" ")[0]);
const run = await new Uploader(db, fetchPost, (fn) => core.exclusive(fn)).drain(input.url, input.token);
process.stdout.write(JSON.stringify({ hello: before, keys, states, run }));
