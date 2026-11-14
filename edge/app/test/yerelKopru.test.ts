// v1 adım 7 (yerel köprü): uygulama tarafı. Gateway bağlantısını Android
// servisi tutar; uygulama açıkken servisin kayıtlarını kendi defterine alır,
// onları ikinci kez göndermez, servisin durumunu "bağlı" olarak gösterir ve
// LoRa komut satırlarını servis üzerinden taşır.
import assert from "node:assert/strict";
import { test } from "node:test";
import { hex } from "../src/core/bytes.ts";
import { SCHEMA } from "../src/core/db.ts";
import { importRows, readCursor, validRow, type BridgeRow } from "../src/core/bridgeImport.ts";
import { benchCredentials, openProductFrame } from "../src/core/product.ts";
import { ownPending, Uploader } from "../src/core/sync.ts";
import { Controller } from "../src/app/controller.ts";
import type { BridgeStatus, GatewayBridgePlugin } from "../src/platform/gatewayBridge.ts";
import { linkStateOf, NativeBleLink } from "../src/platform/nativeBleLink.ts";
import type { LinkState } from "../src/platform/transports.ts";
import { track } from "./fixtures.ts";
import { NodeDb } from "./nodeDb.ts";

const GW = "09A462BD4B275BA5";

async function ledger(): Promise<NodeDb> {
  const db = new NodeDb();
  for (const s of SCHEMA) await db.execute(s);
  return db;
}

/** Rows exactly as BridgeRuntime.changes() sends them (BridgeStore.Change). */
function rowsOf(frames: Uint8Array[], from = 1): BridgeRow[] {
  return frames.map((f, i) => {
    const o = openProductFrame(f, benchCredentials);
    return {
      rev: from + i, conflict: false, device: hex(o.deviceId), incarnation: hex(o.incarnation),
      sequence: o.sequence, family: o.family, plaintext: hex(o.plaintext), frame: hex(o.frame),
      firstSeen: 1_800_000_000 + i, rssi: i % 2 ? -80 : null, snr: i % 2 ? 5 : null, gateway: GW, upload: null,
    };
  });
}

const count = async (db: NodeDb, sql: string) => Number((await db.query(sql))[0].n);

test("köprü kayıtları defterde: ilk kopya kalır, alan bilgisi gelir, imleç ilerler", async () => {
  const db = await ledger();
  const rows = rowsOf(track(5));
  assert.equal(await importRows(db, "AAAA", rows, 5), 5);
  assert.equal(await count(db, "SELECT count(*) AS n FROM observations"), 5);
  assert.equal(await count(db, "SELECT count(*) AS n FROM receipts WHERE gw_rssi IS NOT NULL"), 2);
  assert.equal(await count(db, "SELECT count(*) AS n FROM receipts WHERE gateway='" + GW + "'"), 5);
  assert.equal(await count(db, "SELECT count(*) AS n FROM bridged"), 5);
  assert.deepEqual(await readCursor(db), { db: "AAAA", rev: 5 });
  // The same rows again (the server answered: rev moved) carry their upload state.
  const answered = rows.map((r, i) => ({ ...r, rev: 6 + i, upload: "STORED" }));
  assert.equal(await importRows(db, "AAAA", answered, 10), 0);
  assert.equal(await count(db, "SELECT count(*) AS n FROM uploads WHERE state='STORED'"), 5);
  assert.equal(await count(db, "SELECT count(*) AS n FROM observations"), 5);
  assert.deepEqual(await readCursor(db), { db: "AAAA", rev: 10 });
});

test("köprü kaydı uygulamadakinden farklıysa kanıt olarak saklanır; bozuk satır yazılmaz", async () => {
  const db = await ledger();
  const [r] = rowsOf(track(1));
  await importRows(db, "AAAA", [r], 1);
  const other = { ...r, rev: 2, plaintext: r.plaintext.slice(0, -2) + (r.plaintext.endsWith("00") ? "01" : "00") };
  assert.equal(await importRows(db, "AAAA", [other], 2), 1);
  assert.equal(await count(db, "SELECT count(*) AS n FROM conflicts"), 1);
  assert.equal((await db.query("SELECT plaintext FROM observations"))[0].plaintext, r.plaintext);
  // A conflict row of the bridge itself, twice: kept once.
  const c = { ...other, rev: 3, conflict: true };
  assert.equal(await importRows(db, "AAAA", [c, { ...c, rev: 4 }], 4), 0);
  // Malformed rows never reach a CHECK constraint (would stop every later import).
  for (const bad of [{ ...r, rev: 5, frame: "ZZ" }, { ...r, rev: 5, family: 3 }, { ...r, rev: 5, upload: "MAYBE" },
                     { ...r, rev: 5, device: "zz" + r.device.slice(2) }, { ...r, rev: 5, sequence: 0 }])
    assert.equal(validRow(bad as BridgeRow), false);
  assert.equal(await importRows(db, "AAAA", [{ ...r, rev: 5, frame: "ZZ" }], 5), 0);
  assert.deepEqual(await readCursor(db), { db: "AAAA", rev: 5 });
});

test("uygulama köprünün kayıtlarını ikinci kez göndermez; bekleyen sayısında görünür", async () => {
  const db = await ledger();
  await importRows(db, "AAAA", rowsOf(track(3)), 3);
  const posted: string[] = [];
  const up = new Uploader(db, async (_u, _t, body) => { posted.push(body); return { status: 200, body: { results: [] } }; },
                          (fn) => fn());
  const c = await up.counts();
  assert.deepEqual(c, { uploaded: 0, pending: 3, conflict: 0, rejected: 0, bridged: 3 });
  assert.equal(ownPending(c), 0);
  const r = await up.drain("https://s", "t");
  assert.equal(r.sent, 0);
  assert.equal(posted.length, 0);
  // The bridge reports them uploaded: nothing pending any more.
  await importRows(db, "AAAA", rowsOf(track(3), 4).map((x) => ({ ...x, upload: "DUPLICATE" })), 6);
  assert.deepEqual(await up.counts(), { uploaded: 3, pending: 0, conflict: 0, rejected: 0 });
});

// ---- the link view ------------------------------------------------------------
function fakeBridge(initial: Partial<BridgeStatus>, rows: BridgeRow[] = []) {
  let status: BridgeStatus = { enabled: false, address: null, state: "off", detail: null, loraSeq: 0, ...initial };
  const calls: string[] = [];
  const sent: string[] = [];
  let lines: string[] = [];
  let change: (() => void) | null = null;
  let taken = 0;
  const plugin: GatewayBridgePlugin = {
    async start({ address }) { calls.push("start " + address); status = { ...status, enabled: true, address, state: "connecting" }; },
    async stop() { calls.push("stop"); status = { ...status, enabled: false, state: "off" }; },
    async forget() { calls.push("forget"); status = { ...status, enabled: false, state: "off" }; },
    async resume() { calls.push("resume"); },
    async status() { return status; },
    async takeRecords({ db, after, limit }) {
      const from = db === "DB1" ? after : 0;
      const page = rows.filter((r) => r.rev > from).slice(0, limit ?? 200);
      return { db: "DB1", rows: page, last: page.length ? page[page.length - 1].rev : from };
    },
    async taken({ rev }) { taken = rev; },
    async takeLora({ after }) { return { lines: lines.slice(after), last: lines.length }; },
    async sendLine({ line }) { sent.push(line); },
    async redump() { calls.push("redump"); },
    async uploadNow() { },
    async setReadKeys({ keys }) { calls.push("keys " + keys.length); },
    async addListener(_e, fn) { change = fn; return { remove: async () => { change = null; } }; },
  };
  return {
    plugin, calls, sent,
    set(p: Partial<BridgeStatus>) { status = { ...status, ...p }; change?.(); },
    lora(l: string) { lines = [...lines, l]; status = { ...status, loraSeq: lines.length }; change?.(); },
    get taken() { return taken; },
  };
}

function events() {
  const states: [LinkState, string | undefined][] = [];
  return { states, ev: { log: () => {}, state: (s: LinkState, d?: string) => { states.push([s, d]); }, changed: () => {} } };
}

const settle = () => new Promise((r) => setTimeout(r, 20));

test("uygulama açılınca çalışan köprü 'bağlı' görünür ve kayıtları gelir", async () => {
  const db = await ledger();
  const rows = rowsOf(track(450));
  const b = fakeBridge({ enabled: true, address: "AA:BB:CC:DD:EE:FF", state: "ready", gateway: GW }, rows);
  const { states, ev } = events();
  let imported = 0;
  const link = new NativeBleLink(b.plugin, db, (fn) => fn(), ev, { status: () => {}, imported: (n) => { imported += n; } },
                                 async () => "x", () => null);
  assert.equal(await link.attach(), true);
  assert.deepEqual(states.at(-1), ["connected", undefined]);
  assert.ok(b.calls.includes("resume"));
  for (let i = 0; i < 20 && imported < 450; i++) await settle();
  assert.equal(imported, 450);
  assert.equal(await count(db, "SELECT count(*) AS n FROM observations"), 450);
  assert.equal(b.taken, 450);
  // Gateway out of reach: shown as waiting with the bridge's own words.
  b.set({ state: "waiting", detail: "Gateway'e ulaşılamıyor; menzile girince bağlanacak" });
  await settle();
  assert.deepEqual(states.at(-1), ["waiting", "Gateway'e ulaşılamıyor; menzile girince bağlanacak"]);
  b.set({ state: "adapter_off", detail: null });
  await settle();
  assert.deepEqual(states.at(-1), ["waiting", "Bluetooth kapalı"]);
  await link.stop();
  assert.deepEqual(states.at(-1), ["idle", undefined]);
});

test("köprü kapalıysa açılışta bağlantı yok; seçilen gateway servise verilir", async () => {
  const db = await ledger();
  const b = fakeBridge({});
  const { states, ev } = events();
  const link = new NativeBleLink(b.plugin, db, (fn) => fn(), ev, { status: () => {}, imported: () => {} },
                                 async (pick, id) => (pick ? "picked" : id ?? "remembered"), () => null);
  assert.equal(await link.attach(), false);
  assert.deepEqual(states.at(-1), ["idle", undefined]);
  await link.start(false, "aa:bb:cc:dd:ee:ff");
  assert.deepEqual(b.calls.filter((c) => c.startsWith("start")), ["start AA:BB:CC:DD:EE:FF"]);
  assert.equal(states.at(-1)?.[0], "connecting");
});

test("LoRa komut satırları servis üzerinden gider, gateway'in cevapları uygulamaya gelir", async () => {
  const db = await ledger();
  const b = fakeBridge({ enabled: true, address: "A", state: "ready", gateway: GW });
  b.lora("LORA QUEUED result=ADDED");                        // before this app run: not replayed
  const { ev } = events();
  const reported: unknown[] = [];
  const server = {
    loraCommands: async () => [{ device: "1122334455667788", command_id: 7, frames: ["AB".repeat(60)] }],
    loraEvents: async (e: unknown[]) => { reported.push(...e); },
  };
  const link = new NativeBleLink(b.plugin, db, (fn) => fn(), ev, { status: () => {}, imported: () => {} },
                                 async () => "x", () => server);
  await link.attach();
  await new Promise((r) => setTimeout(r, 2300));
  assert.equal(b.sent.length, 1);
  assert.match(b.sent[0], /^APP LORA QUEUE [0-9A-F]+$/);
  b.lora("LORA SENT target=1122334455667788 id=00000007 attempt=1");
  await settle();
  await new Promise((r) => setTimeout(r, 2100));
  assert.deepEqual(reported, [{ kind: "sent", device: "1122334455667788", command_id: 7 }]);
  await link.stop();
});

test("köprü durumu uygulamanın bağlantı durumuna doğru çevrilir", () => {
  const base = { enabled: true, address: "A", detail: null } as const;
  assert.deepEqual(linkStateOf({ ...base, state: "ready" }), { state: "connected", detail: null });
  assert.deepEqual(linkStateOf({ ...base, state: "ready", keyMissing: true }).state, "connected");
  assert.match(String(linkStateOf({ ...base, state: "ready", keyMissing: true }).detail), /Teslim anahtarı yok/);
  assert.deepEqual(linkStateOf({ ...base, state: "connecting" }), { state: "connecting", detail: null });
  assert.equal(linkStateOf({ ...base, state: "off" }).state, "connecting");     // service still starting
  assert.deepEqual(linkStateOf({ ...base, enabled: false, state: "ready" }), { state: "idle", detail: null });
});

test("denetleyici: Bluetooth sayaçları köprüden gelir, bildirimi köprü gösterir", async () => {
  const c = new Controller(null);
  const core = {
    counts: { STORED: 0, DUPLICATE: 0, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 }, handedOff: 0, acceptRejected: 0,
    gatewayId: null, lastHello: null, acceptKeyMissing: false, pendingAccepts: () => 0, heldNow: () => null,
    ledger: { recordCount: async () => 42 },
  };
  let keepAliveOn = 0;
  Object.assign(c as unknown as Record<string, unknown>, {
    core, native: {}, keepAlive: { on: () => { keepAliveOn++; }, off: () => {} }, herdWatch: () => {},
  });
  (c as unknown as { bridgeStatus: (s: BridgeStatus) => void }).bridgeStatus({
    enabled: true, address: "A", state: "ready", detail: null, gateway: GW, held: 3, capacity: 1056,
    counts: { STORED: 7, DUPLICATE: 1, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 }, handedOff: 6, acceptRejected: 0,
    pendingAccepts: 1,
  });
  await new Promise((r) => setTimeout(r, 400));
  assert.equal(c.snap.link, "ble");
  assert.equal(c.snap.counts.STORED, 7);
  assert.equal(c.snap.handedOff, 6);
  assert.equal(c.snap.pending, 1);
  assert.equal(c.snap.gateway, GW);
  assert.equal(c.snap.gatewayHeld, "3/1056");
  assert.equal(c.snap.records, 42);
  assert.equal(keepAliveOn, 0);
  assert.ok(c.snap.journal.some((j) => /7 yeni kayıt/.test(j.text)));
});
