// Y2a-2: tek ORUN yazılımı — boş cihaza kurulumda rol (tasma / gateway), GPS'e göre öneri.
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  chooseRole, NoRoleError, noRoleMessage, readTrackerReport, reopenAfterRestart,
  setRoleAndReconnect, suggestedRole, type ClosableConsole,
} from "../src/app/addTracker.ts";

/** Satır konsolu: her gönderilen satıra verilen yanıtları sıraya koyar. */
class Fake implements ClosableConsole {
  sent: string[] = [];
  closed = false;
  private queue: string[] = [];
  private answer: (line: string) => string[];
  constructor(answer: (line: string) => string[]) { this.answer = answer; }
  async send(line: string) { this.sent.push(line); this.queue.push(...this.answer(line)); }
  async waitFor<T>(match: (l: string) => T | null): Promise<T | null> {
    while (this.queue.length) { const r = match(this.queue.shift() as string); if (r !== null) return r; }
    return null;
  }
  async close() { this.closed = true; }
}

const BLANK = "BENCH DEVICE id=52E16773141A8887 role=UNASSIGNED security_region=ERASED gnss=yes";
const blankDevice = () => new Fake((l) => l === "BENCH?" ? [BLANK]
  : l === "APP SERVICES T" || l === "APP SERVICES A"
    ? ["ROLE chosen=" + (l.endsWith("T") ? "TRACKER" : "GATEWAY") + "; restarting into it",
       "APP SET id=7 code=APPLIED services=" + l.slice(-1) + " mode=EXPLICIT"]
    : []);
const noSleep = async () => {};

test("öneri: GPS varsa tasma, yoksa gateway, bilinmiyorsa yok", () => {
  assert.equal(suggestedRole("yes"), "tasma");
  assert.equal(suggestedRole("no"), "gateway");
  assert.equal(suggestedRole("unknown"), null);
  assert.match(noRoleMessage("UNASSIGNED"), /kurulmamış/);
});

test("kurulmamış cihaz tasma eklerken rol sorusuna yönlendirir", async () => {
  await assert.rejects(readTrackerReport(blankDevice()), (e: unknown) =>
    e instanceof NoRoleError && e.id === "52E16773141A8887" && e.gnss === "yes" && !e.conflict);
});

test("rol seçimi: cihaz kabul eder ya da rolü belli cihaz reddeder", async () => {
  const d = blankDevice();
  await chooseRole(d, "tasma");
  assert.deepEqual(d.sent, ["APP SERVICES T"]);
  const g = blankDevice();
  await chooseRole(g, "gateway");
  assert.deepEqual(g.sent, ["APP SERVICES A"]);
  // Kurulmuş tasma gateway olamaz.
  const set = new Fake(() => ["APP SET id=0 code=INVALID reason=device_is_TRACKER"]);
  await assert.rejects(chooseRole(set, "gateway"), /rolü zaten belli/);
  // Rol değişmeden kaydedildiyse (ROLE chosen yok) de başarı sayılmaz.
  const quiet = new Fake(() => ["APP SET id=3 code=APPLIED services=T mode=EXPLICIT"]);
  await assert.rejects(chooseRole(quiet, "tasma"), /rolü zaten belli/);
  await assert.rejects(chooseRole(new Fake(() => []), "tasma"), /yanıt vermedi/);
});

test("yeniden başlarken USB birkaç kez denenir; izin reddi beklemeden durur", async () => {
  let n = 0;
  const got = await reopenAfterRestart(async () => { if (++n < 3) throw new Error("USB'de cihaz yok"); return "açık"; },
    noSleep, { tries: 5 });
  assert.equal(got, "açık");
  assert.equal(n, 3);
  n = 0;
  await assert.rejects(reopenAfterRestart(async () => { ++n; throw new Error("USB izni verilmedi"); }, noSleep),
    /izni verilmedi/);
  assert.equal(n, 1);
  await assert.rejects(reopenAfterRestart(async () => { throw new Error("yok"); }, noSleep, { tries: 2 }),
    /USB açılamadı/);
});

test("rol yazılır, cihaz yeniden başlar, seçilen rolde açıldığı doğrulanır", async () => {
  const first = blankDevice();
  const tracker = new Fake((l) => l === "BENCH?" ? [
    "BENCH DEVICE id=52E16773141A8887 role=TRACKER security_region=ERASED gnss=yes",
    "BENCH CREDENTIAL credential=none epoch=00000000 grant_generation=00000000 custody_floor=00000000",
    "BENCH TRACKER id=52E16773141A8887 security=UNPROVISIONED test_credential=no page=v0 granted_gateway=none setup=IDLE role=TRACKER product_store=mounted",
  ] : []);
  const r = await setRoleAndReconnect(first, "tasma", async () => tracker, noSleep);
  assert.ok(first.closed && !tracker.closed);
  assert.equal(r.device.kind, "tracker");
  // Gateway seçilip cihaz tasma olarak açılırsa: hata, bağlantı kapanır.
  const wrong = new Fake((l) => l === "BENCH?" ? [
    "BENCH CREDENTIAL credential=none epoch=00000000 grant_generation=00000000 custody_floor=00000000",
    "BENCH TRACKER id=52E16773141A8887 granted_gateway=none",
  ] : []);
  await assert.rejects(setRoleAndReconnect(blankDevice(), "gateway", async () => wrong, noSleep), /seçilen rolle açılmadı/);
  assert.ok(wrong.closed);
  const gw = new Fake((l) => l === "BENCH?" ? ["BENCH GATEWAY id=52E16773141A8887 custody_region=READY (signed grants only)"] : []);
  assert.deepEqual((await setRoleAndReconnect(blankDevice(), "gateway", async () => gw, noSleep)).device,
    { kind: "gateway", id: "52E16773141A8887" });
});
