// 1d/1e: uygulamadan tasma ekle — tasma raporu, kurulum satırları, onay.
import assert from "node:assert/strict";
import { test } from "node:test";
import { addTrackerOverUsb, type LineConsole, type SetupServer } from "../src/app/addTracker.ts";

const DEV = "0E8ADE7E71531AA3";
const GW = "09A462BD4B275BA5";
const CRED = "AB".repeat(16);

/** Answers like the real Tracker image over USB. */
class FakeTracker implements LineConsole {
  sent: string[] = [];
  private queue: string[] = [];
  private provisioned = false;
  private reply: "pass" | "fail";
  constructor(reply: "pass" | "fail" = "pass") { this.reply = reply; }
  async send(line: string): Promise<void> {
    this.sent.push(line);
    if (line === "BENCH?") {
      this.queue.push(this.provisioned
        ? `BENCH CREDENTIAL credential=${CRED} epoch=00000001 grant_generation=00000001 custody_floor=00000000`
        : "BENCH CREDENTIAL credential=none epoch=00000000 grant_generation=00000000 custody_floor=00000000");
      this.queue.push(`BENCH TRACKER id=${DEV} security=PROVISIONED test_credential=no page=v3 granted_gateway=${this.provisioned ? GW : "none"} setup=DONE role=TRACKER product_store=mounted`);
    } else if (line.startsWith("BENCH PROVISION ")) {
      this.queue.push("BENCH PROVISION started step=CREDENTIAL", "BENCH SETUP step=MIGRATE_V3");
      if (this.reply === "pass") { this.provisioned = true; this.queue.push("BENCH SETUP PASS"); }
      else this.queue.push("BENCH SETUP FAIL reason=no available Gateway grant slot");
    }
  }
  async waitFor<T>(match: (line: string) => T | null): Promise<T | null> {
    while (this.queue.length) {
      const got = match(this.queue.shift() as string);
      if (got !== null) return got;
    }
    return null;
  }
}

function server(log: unknown[]): SetupServer {
  return {
    async devicePost(path, body) {
      log.push([path, body]);
      if (path.endsWith("/prepare"))
        return { lines: [`BENCH PROVISION ${CRED} 00000001 ${"00".repeat(32)} ${GW} 00000000`], plan: [] };
      return { device: DEV, gateways: [GW], added: 1 };
    },
  };
}

test("tasma okunur, satır yazılır, tasmanın yeni raporuyla onaylanır", async () => {
  const con = new FakeTracker();
  const log: unknown[] = [];
  const steps: string[] = [];
  const done = await addTrackerOverUsb(con, server(log), (s) => steps.push(s));
  assert.deepEqual(done, { device: DEV, gateways: [GW], added: 1 });
  assert.deepEqual(steps, ["okunuyor", "hazırlanıyor", "yazılıyor", "onaylanıyor"]);
  const [[, before], [, after]] = log as [string, Record<string, unknown>][];
  assert.equal(before.credential, "none");
  assert.equal(after.granted, GW);
  assert.equal(after.generation, 1);
  assert.equal(con.sent.filter((l) => l.startsWith("BENCH PROVISION")).length, 1);
});

test("tasma reddederse hata açık söylenir, onay gönderilmez", async () => {
  const log: unknown[] = [];
  await assert.rejects(addTrackerOverUsb(new FakeTracker("fail"), server(log)),
    /no available Gateway grant slot/);
  assert.equal(log.length, 1);   // yalnız hazırlık
});

test("gateway bağlıysa tasma gibi eklenmez", async () => {
  const gw: LineConsole = {
    async send() {},
    async waitFor(match) { return match(`BENCH GATEWAY id=${GW} held=0`); },
  };
  await assert.rejects(addTrackerOverUsb(gw, server([])), /Bu bir gateway/);
});
