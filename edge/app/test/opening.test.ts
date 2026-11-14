// Açılışta ve öne gelişte eski zamanın "güncel" gösterilmemesi (src/app/opening.ts),
// sahibin telefonundaki sırayla adım adım.
import assert from "node:assert/strict";
import { test } from "node:test";
import { REFRESH_LIMIT_MS, refreshDone, refreshing, type OpeningInput } from "../src/app/opening.ts";
import { buildHerd } from "../src/app/herd.ts";
import { pt } from "./herd.test.ts";

const base: OpeningInput = {
  accountSettled: false, signedIn: false, from: 0, now: 0,
  latestAt: null, lastAt: null, errorAt: null, screenCurrent: false, doneFor: -1,
};
const at = (patch: Partial<OpeningInput>) => ({ ...base, ...patch });

test("opening, step by step: old times are not shown until the newest records are on screen", () => {
  // 0 ms: app opened, account not read yet.
  assert.equal(refreshing(at({ now: 0 })), true);
  // 300 ms: account restored; the phone ledger (7 min old) is on screen.
  const restored = { accountSettled: true, signedIn: true, now: 300, screenCurrent: true };
  assert.equal(refreshing(at(restored)), true, "telefondaki kayıt 7 dk eski: henüz güncel değil");
  // 900 ms: the sync wrote every tracker's newest record; the screen re-reads.
  const written = { ...restored, now: 900, latestAt: 900, screenCurrent: false };
  assert.equal(refreshing(at(written)), true, "yazıldı ama ekran eskisini gösteriyor");
  // 950 ms: the screen shows them -> done.
  const shown = { ...written, now: 950, screenCurrent: true };
  assert.equal(refreshDone(at(shown)), true);
  assert.equal(refreshing(at(shown)), false);
  // 1.5 s: history pages arrive and the screen re-reads again: not "yenileniyor" again.
  assert.equal(refreshing(at({ ...shown, now: 1500, screenCurrent: false, doneFor: 0 })), false);
});

test("coming back after a minute in the background starts a new refresh", () => {
  const before = { accountSettled: true, signedIn: true, latestAt: 900, lastAt: 2000, screenCurrent: true, doneFor: 0 };
  const from = 70_000;
  assert.equal(refreshing(at({ ...before, from, now: from })), true, "eski eşitleme bu dönüşü saymaz");
  assert.equal(refreshing(at({ ...before, from, now: from + 800, latestAt: from + 700 })), false);
});

test("no internet, slow link, no account: what the phone holds is shown", () => {
  const signed = { accountSettled: true, signedIn: true, screenCurrent: true };
  assert.equal(refreshing(at({ ...signed, now: 400, errorAt: 350 })), false, "internet yok: eldeki en yenisi");
  assert.equal(refreshing(at({ ...signed, now: REFRESH_LIMIT_MS })), false, "10 sn sınırı");
  assert.equal(refreshing(at({ accountSettled: true, signedIn: false, now: 100 })), false, "hesap yok");
  // An old failure does not end a new refresh.
  assert.equal(refreshing(at({ ...signed, from: 70_000, now: 70_100, errorAt: 5_000 })), true);
});

test("while refreshing, the herd says 'yenileniyor' instead of an old time; real problems stay", () => {
  const NOW = Date.parse("2026-10-25T12:00:00Z");
  const pts = [pt("c1", 7, { period_s: 300 })];
  const [plain] = buildHerd({ points: pts, profiles: {}, assetOf: (d) => d, now: NOW });
  assert.match(plain.seen, /7 dk önce/);
  const [upd] = buildHerd({ points: pts, profiles: {}, assetOf: (d) => d, now: NOW, settling: true });
  assert.equal(upd.status, "ok");
  assert.equal(upd.seen, "Konum yenileniyor…");
  assert.equal(upd.short, "yenileniyor");
  assert.equal(upd.freshRatio, 1);
  // Battery critical is not a time: it stays.
  const [bat] = buildHerd({ points: [pt("c1", 7, { period_s: 300, battery_mv: 3405, battery: "CRITICAL" })],
    profiles: {}, assetOf: (d) => d, now: NOW, settling: true });
  assert.equal(bat.status, "battery-critical");
  assert.equal(bat.short, "Pil bitiyor");
});
