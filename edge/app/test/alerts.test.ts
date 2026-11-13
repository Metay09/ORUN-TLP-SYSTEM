// 5. adım: hangi olay telefona bildirim olur (src/app/alerts.ts).
import assert from "node:assert/strict";
import { test } from "node:test";
import { EMPTY_MEMORY, nextAlerts, watchFresh } from "../src/app/alerts.ts";
import type { AppEvent, EventKind } from "../src/app/model.ts";

const NOW = Date.parse("2026-11-13T12:00:00Z");
const MIN = 60_000;
function ev(kind: EventKind, agoMin: number, endAgoMin: number | null = null, extra: Partial<AppEvent> = {}): AppEvent {
  const at = NOW - agoMin * MIN;
  const title = kind === "out" ? "Sarıkız alan dışına çıktı" : kind === "lost" ? "Sarıkız kayıp olabilir"
    : kind === "silent" ? "Sarıkız sessiz" : "Sarıkız: pil bitiyor";
  return { id: `${kind}:A:${at}`, kind, severity: "danger", asset: "A", device: "A", title, detail: "ayrıntı",
           at, end: endAgoMin === null ? null : NOW - endAgoMin * MIN, arrivedAt: null, alert: true, ...extra };
}
const ready = { ready: true, keys: [] as string[] };

test("first run tells nothing and remembers what is already there", () => {
  const events = [ev("out", 30), ev("battery-critical", 500)];
  const first = nextAlerts(events, EMPTY_MEMORY, NOW);
  assert.equal(first.alerts.length, 0);
  assert.equal(first.memory.ready, true);
  assert.equal(nextAlerts(events, first.memory, NOW).alerts.length, 0);
});

test("each of the four is told once", () => {
  const events = [ev("out", 10), ev("silent", 50), ev("lost", 100), ev("battery-critical", 5)];
  const a = nextAlerts(events, ready, NOW);
  assert.deepEqual(a.alerts.map((x) => x.title).sort(), [
    "Sarıkız alan dışına çıktı", "Sarıkız kayıp olabilir", "Sarıkız sessiz", "Sarıkız: pil bitiyor"].sort());
  assert.equal(nextAlerts(events, a.memory, NOW + MIN).alerts.length, 0);
});

test("back inside is told only after the leaving was told", () => {
  const leaving = ev("out", 40);
  const told = nextAlerts([leaving], ready, NOW);
  assert.equal(told.alerts.length, 1);
  const closed = { ...leaving, end: NOW - 5 * MIN };
  const back = nextAlerts([closed], told.memory, NOW);
  assert.equal(back.alerts.length, 1);
  assert.equal(back.alerts[0].title, "Sarıkız alana döndü");
  assert.match(back.alerts[0].body, /35 dk dışarıda kaldı/);
  assert.equal(nextAlerts([closed], back.memory, NOW).alerts.length, 0);
  // A period that opened and closed while nobody was told (late backlog): silent.
  assert.equal(nextAlerts([ev("out", 90, 60)], ready, NOW).alerts.length, 0);
});

test("book lines, positionless reports and other kinds are not notifications; an old open one is", () => {
  const book = ev("silent", 10, null, { alert: false });
  const nofix = ev("silent", 10); nofix.id = `nofix:A:${nofix.at}`;
  const low = ev("battery-low" as EventKind, 10);
  const still = ev("still" as EventKind, 10);
  assert.equal(nextAlerts([book, nofix, low, still], ready, NOW).alerts.length, 0);
  // Left 20 h ago while the phone was off and is still outside: told.
  assert.equal(nextAlerts([ev("out", 20 * 60)], ready, NOW).alerts.length, 1);
  // Low battery that turned critical keeps its id (events.ts) and is told then.
  const turned = { ...low, kind: "battery-critical" as EventKind };
  const m = nextAlerts([low], ready, NOW).memory;
  assert.equal(nextAlerts([turned], m, NOW).alerts.length, 1);
});

test("stale phone data decides nothing", () => {
  assert.equal(watchFresh(null, false, NOW), false);
  assert.equal(watchFresh({ at: NOW - 60_000, more: false }, false, NOW), true);
  assert.equal(watchFresh({ at: NOW - 60_000, more: true }, false, NOW), false);   // history still arriving
  assert.equal(watchFresh({ at: NOW - 6 * 60_000, more: false }, false, NOW), false);
  assert.equal(watchFresh(null, true, NOW), true);                                // gateway attached
});

test("silent that becomes lost is a second notification; memory stays bounded", () => {
  const s = ev("silent", 50);
  const a = nextAlerts([s], ready, NOW);
  const l = { ...s, kind: "lost" as EventKind, id: `lost:A:${s.at}`, title: "Sarıkız kayıp olabilir" };
  const b = nextAlerts([l], a.memory, NOW + 60 * MIN);
  assert.equal(b.alerts.length, 1);
  let memory = ready;
  for (let i = 0; i < 1000; i++) memory = nextAlerts([ev("out", 1, null, { id: `out:A:${i}` })], memory, NOW).memory;
  assert.ok(memory.keys.length <= 400);
});

test("the 'back inside' title is built from the real 'left the zone' wording", async () => {
  const { tr } = await import("../src/app/i18n/tr.ts");
  const leaving = { ...ev("out", 40), title: tr.events.out.title("Sarıkız") };
  const told = nextAlerts([leaving], ready, NOW);
  const back = nextAlerts([{ ...leaving, end: NOW - MIN }], told.memory, NOW);
  assert.equal(back.alerts[0].title, "Sarıkız alana döndü");
});
