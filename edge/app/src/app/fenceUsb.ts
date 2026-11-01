// 3a: alanları USB ile bağlı tasmaya yaz (tools/orun_fence_usb.py ile aynı
// protokol). Tasma tam listeyi alır (eskisinin yerine geçer); boş liste hiçbir
// zaman "hepsini sil" sayılmaz. Başarı yalnız tasmanın onayı ve yeniden okunan
// durumla kabul edilir.
import { sha256 } from "@noble/hashes/sha2.js";
import type { LineConsole } from "./addTracker.ts";
import { canonicalE7, FENCE, type E7, type Zone } from "./zones.ts";

const RESULT = /^APP FENCE id=\d+ code=(\w+) state=(\w+) areas=(\d+) vertices=(\d+) token=(\w+)/;
const STAGED = /^APP FENCE STAGING areas=(\d+) vertices=(\d+)$/;
const STATUS = /^APP GEOFENCE id=\d+ resource=\d+ token=\d+ areas=(\d+) vertices=(\d+) runtime=(\w+)/;
const REJECT = /^(APP FENCE REJECTED|APP REJECTED|APP BUSY)/;

export interface FenceSummary { areas: number; vertices: number; digest: string; }

/** "376107000" → "37.6107000" (tam sayıyla, yuvarlama hatası yok). */
export function decimalE7(n: number): string {
  const sign = n < 0 ? "-" : "";
  const a = Math.abs(n);
  return `${sign}${Math.floor(a / 1e7)}.${String(a % 1e7).padStart(7, "0")}`;
}

export function polygonsOf(zones: Zone[]): E7[][] {
  return zones.map((z) => canonicalE7(z.ring));
}

/** Same content → same digest: tells whether a tracker's zones are current. */
export function fenceDigest(polys: E7[][]): string {
  const text = polys.map((p) => p.map(([la, lo]) => `${la},${lo}`).join(";")).join("|");
  return Array.from(sha256(new TextEncoder().encode(text)).slice(0, 8), (b) => b.toString(16).padStart(2, "0")).join("");
}

export function summary(polys: E7[][]): FenceSummary {
  return { areas: polys.length, vertices: polys.reduce((n, p) => n + p.length, 0), digest: fenceDigest(polys) };
}

/** Lines to send, with the staging counts the tracker must answer. */
export function fenceCommands(polys: E7[][]): { line: string; area: number; vertices: number }[] {
  const out = [{ line: "APP FENCE BEGIN", area: 1, vertices: 0 }];
  let total = 0;
  polys.forEach((poly, i) => {
    if (i) out.push({ line: "APP FENCE AREA", area: i + 1, vertices: total });
    for (const [lat, lon] of poly) {
      total += 1;
      out.push({ line: `APP FENCE V ${decimalE7(lat)} ${decimalE7(lon)}`, area: i + 1, vertices: total });
    }
  });
  return out;
}

async function expect<T>(con: LineConsole, match: (l: string) => T | null, ms: number): Promise<T> {
  let rejected: string | null = null;
  const got = await con.waitFor((line) => {
    if (REJECT.test(line)) { rejected = line; return true as unknown as T; }
    return match(line);
  }, ms);
  if (rejected) throw new Error("Tasma reddetti: " + rejected);
  if (got === null) throw new Error("Tasmadan onay gelmedi; alanların yazıldığını varsayma.");
  return got;
}

/** Write the whole list to the tracker on `con`, then read it back. */
export async function writeFence(con: LineConsole, zones: Zone[],
                                 onStep: (done: number, total: number) => void = () => {}): Promise<FenceSummary> {
  const polys = polygonsOf(zones);
  const sum = summary(polys);
  if (!sum.areas) throw new Error("Bu tasmaya gidecek alan yok.");
  if (sum.areas > FENCE.maxAreas || sum.vertices > FENCE.maxTotalVertices)
    throw new Error(`Tasma en fazla ${FENCE.maxAreas} alan ve ${FENCE.maxTotalVertices} köşe alır.`);
  await con.send("APP GEOFENCE?");
  await expect(con, (l) => STATUS.exec(l), 8000);
  const steps = fenceCommands(polys);
  let committed = false;
  try {
    for (let i = 0; i < steps.length; i++) {
      const s = steps[i];
      await con.send(s.line);
      await expect(con, (l) => {
        const m = STAGED.exec(l);
        return m && Number(m[1]) === s.area && Number(m[2]) === s.vertices ? m : null;
      }, 5000);
      onStep(i + 1, steps.length + 1);
    }
    committed = true;
    await con.send("APP FENCE COMMIT");
    const r = await expect(con, (l) => RESULT.exec(l), 25_000);
    if (!["APPLIED", "UNCHANGED"].includes(r[1]) || r[2] !== "CONFIGURED" ||
        Number(r[3]) !== sum.areas || Number(r[4]) !== sum.vertices || r[5] !== "VALID")
      throw new Error("Tasma alanları kabul ettiğini doğrulamadı: " + r[0]);
    await con.send("APP GEOFENCE?");
    const st = await expect(con, (l) => STATUS.exec(l), 8000);
    if (Number(st[1]) !== sum.areas || Number(st[2]) !== sum.vertices || st[3] !== "configured")
      throw new Error("Tasmadan yeniden okunan alan sayısı tutmuyor: " + st[0]);
    onStep(steps.length + 1, steps.length + 1);
    return sum;
  } catch (e) {
    if (!committed) { try { await con.send("APP FENCE ABORT"); } catch { /* gone */ } }
    throw e;
  }
}
