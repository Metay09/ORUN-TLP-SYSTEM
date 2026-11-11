// 6c: telefon köprüsü LoRa komutlarını gateway'e verir, gateway'in dediğini bildirir.
import assert from "node:assert/strict";
import { test } from "node:test";
import { CHECK_MS, LoraCarrier, loraLines, REHAND_MS, type LoraCommand, type LoraEvent } from "../src/app/loraCarrier.ts";

const DEV = "52E16773141A8887";
const HEX = "AB".repeat(54);

test("komut bir kez verilir; gateway'in dedikleri sunucuya gider", async () => {
  const sent: LoraEvent[][] = [];
  const commands: LoraCommand[] = [{ device: DEV, resource: 1, command_id: 7, frames: [HEX] }];
  const server = { loraCommands: async () => commands, loraEvents: async (e: LoraEvent[]) => { sent.push(e); } };
  const lines: string[] = [];
  const write = async (l: string) => { lines.push(l); };
  const c = new LoraCarrier();
  await c.poll(server, write, 1000);
  assert.deepEqual(lines, [`APP LORA QUEUE ${HEX}`]);
  await c.poll(server, write, 1000 + CHECK_MS);
  assert.equal(lines.length, 1, "aynı komut tekrar verilmez");
  c.observe("LORA QUEUED result=ADDED waiting=1");
  c.observe(`LORA SENT target=${DEV} id=00000007 attempt=1/6`);
  c.observe("LORA RESULT queued=yes rssi=-40 hex=" + "CD".repeat(56));
  await c.poll(server, write, 1000 + CHECK_MS + 1);
  assert.deepEqual(sent[0].map((e) => e.kind), ["queued", "sent", "result"]);
  assert.deepEqual(sent[0][0], { kind: "queued", device: DEV, command_id: 7 });
  await c.poll(server, write, 1000 + REHAND_MS);
  assert.equal(lines.length, 2, "uzun süre sonra yeniden verilir");
  c.reset();
  await c.poll(server, write, 1000 + REHAND_MS + CHECK_MS);
  assert.equal(lines.length, 3, "yeniden bağlanan gateway'e yeniden verilir");
});

test("internet yokken olaylar bekler, sonra gider; bozuk komut verilmez", async () => {
  let online = false;
  const sent: LoraEvent[][] = [];
  const server = {
    loraCommands: async () => [{ device: DEV, command_id: 1, frames: ["XYZ"] }] as LoraCommand[],
    loraEvents: async (e: LoraEvent[]) => { if (!online) throw new Error("yok"); sent.push(e); },
  };
  const lines: string[] = [];
  const c = new LoraCarrier();
  c.observe(`LORA DROPPED target=${DEV} id=00000001`);
  await c.poll(server, async (l) => { lines.push(l); }, 0);
  assert.equal(sent.length, 0);
  online = true;
  await c.poll(server, async (l) => { lines.push(l); }, 1);
  assert.equal(sent[0][0].kind, "dropped");
  assert.equal(lines.length, 0);
  await c.poll(null, async (l) => { lines.push(l); }, 2);   // hesaba girilmemiş: hiçbir şey
});

test("uzun çerçeve parça parça satırla gider; satırlar kısa kalır", () => {
  const frame = "AB".repeat(255);
  const lines = loraLines(frame);
  assert.ok(lines.every((l) => l.length <= 160));
  assert.equal(lines.slice(0, -1).every((l) => l.startsWith("APP LORA + ")), true);
  assert.ok(lines[lines.length - 1].startsWith("APP LORA QUEUE "));
  assert.equal(lines.map((l) => l.split(" ").pop()).join(""), frame);
  assert.deepEqual(loraLines("AB".repeat(54)), [`APP LORA QUEUE ${"AB".repeat(54)}`]);
});
