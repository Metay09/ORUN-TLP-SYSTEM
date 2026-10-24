// Cross-check CLI for firmware/tests/sf5/test_sf5l7_edge_app.py.
// stdin: {"frames": [hex], "macs": [[gw, page, slot, gen, objHex]],
//         "script": [[t, line] | [t, "@alive"]]}
// stdout: the TypeScript core's view of the same inputs, as JSON.
import { readFileSync } from "node:fs";
import { fromHex, hex } from "../src/core/bytes.ts";
import { EdgeCore } from "../src/core/edge.ts";
import { acceptLine, acceptMac } from "../src/core/edgeLink.ts";
import { benchCredentials, describePeriodic, human, openProductFrame } from "../src/core/product.ts";
import { NodeDb } from "./nodeDb.ts";

const input = JSON.parse(readFileSync(0, "utf8"));

const opened = (input.frames as string[]).map((h) => {
  try {
    const o = openProductFrame(fromHex(h), benchCredentials);
    return {
      ok: true, device: hex(o.deviceId), incarnation: hex(o.incarnation), sequence: o.sequence,
      family: o.family, plaintext: hex(o.plaintext),
      human: o.family === 1 ? human(describePeriodic(o.deviceId, o.plaintext)) : null,
    };
  } catch {
    return { ok: false };
  }
});

const macs = (input.macs as [string, number, number, string, string][]).map(
  ([gw, page, slot, gen, obj]) => ({
    mac: hex(acceptMac(BigInt("0x" + gw), page, slot, BigInt("0x" + gen), fromHex(obj))),
    line: acceptLine(BigInt("0x" + gw), page, slot, BigInt("0x" + gen), fromHex(obj)),
  }));

let now = 0;
const edge = new EdgeCore(new NodeDb(), { credentials: benchCredentials, clock: () => now });
await edge.init();
const outputs: (string | null)[] = [];
const accepts: string[] = [];
for (const [t, line] of input.script as [number, string][]) {
  now = t;
  if (line === "@alive") { await edge.sessionAlive("usb"); continue; }
  if (line !== "@tick") outputs.push(await edge.handleLine(line));   // @tick: time only
  const a = edge.nextAcceptLine(now);
  if (a) accepts.push(a);
}
const points = await edge.points();
process.stdout.write(JSON.stringify({
  opened, macs, outputs, accepts, summary: await edge.summary(), points: points.points,
  skipped: points.skipped,
}));
