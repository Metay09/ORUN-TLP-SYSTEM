// Olaylar özeti (app/digest.ts): ham olaylar hayvan ve tür bazında birleşir.
import assert from "node:assert/strict";
import test from "node:test";
import { digest } from "../src/app/digest.ts";
import type { Animal, AppEvent, EventKind, Severity } from "../src/app/model.ts";

const NOW = new Date(2026, 10, 10, 18, 0, 0).getTime();
const at = (h: number, m = 0, dayBack = 0) => new Date(2026, 10, 10 - dayBack, h, m, 0).getTime();
const animal = (id: string, name: string, batteryPercent: number | null = 80) =>
  ({ id, name, batteryPercent, last: null } as unknown as Animal);
let n = 0;
const ev = (kind: EventKind, severity: Severity, asset: string | null, start: number, end: number | null, alert = true, title = ""): AppEvent =>
  ({ id: `e${n++}`, kind, severity, asset, device: null, title, detail: "", at: start, end, arrivedAt: null, alert });
const HERD = [animal("A", "Duman"), animal("B", "Pamuk", 5), animal("C", "Benekli")];

test("şimdi: bir hayvanın süren sorunları tek kartta, en ağırı önce", () => {
  const d = digest([
    ev("battery-low", "warn", "A", at(9), null),
    ev("lost", "danger", "A", at(13), null),
    ev("battery-critical", "danger", "B", at(17), null),
  ], HERD, NOW);
  assert.equal(d.now.length, 2);
  const duman = d.now.find((i) => i.asset === "A")!;
  assert.deepEqual(duman.reasons, ["5 saattir haber yok", "pil %80, azalıyor"]);
  assert.equal(duman.severity, "danger");
  assert.equal(duman.ids.length, 2);
  assert.equal(duman.advice, "Son görüldüğü yere bak.");
  assert.equal(d.now[0].asset, "A", "daha eski başlayan tehlike önce");
  assert.deepEqual(d.now[1].reasons, ["pil %5, bitiyor"]);
});

test("gün: aynı hayvanın kesintileri tek satırda birleşir", () => {
  const d = digest([
    ev("silent", "warn", "C", at(8), at(9)),
    ev("silent", "warn", "C", at(11), at(13, 30)),
    ev("still", "warn", "A", at(2), at(5, 20)),
  ], HERD, NOW);
  const today = d.days[0];
  assert.equal(today.label, "Bugün");
  assert.equal(today.headline, "2 hayvanda olay · 1 hayvan sorunsuz");
  const benekli = today.lines.find((l) => l.asset === "C")!;
  assert.equal(benekli.text, "2 kez haber kesildi, toplam 3 sa 30 dk; en uzunu 11:00–13:30");
  assert.equal(today.lines.find((l) => l.asset === "A")!.text, "3 sa 20 dk hareketsiz kaldı (02:00–05:20)");
});

test("gün: süren olay günlük satır olmaz; bugün boş olsa da görünür", () => {
  const d = digest([ev("lost", "danger", "A", at(13), null)], HERD, NOW);
  assert.equal(d.days.length, 1);
  assert.equal(d.days[0].lines.length, 0);
  assert.equal(d.days[0].headline, "Sorun görülmedi");
});

test("sistem: tür başına tek satır, en yeni metin ve kaç kez", () => {
  const d = digest([
    ev("link", "info", null, at(6), at(6), false, "Gateway'e bağlanıldı"),
    ev("link", "info", null, at(9), at(9), false, "Gateway bağlantısı koptu"),
    ev("upload", "info", null, at(10), at(10), false, "214 kayıt sunucuya gitti"),
    ev("link", "info", null, at(7, 0, 1), at(7, 0, 1), false, "Gateway'e bağlanıldı"),
  ], HERD, NOW);
  assert.deepEqual(d.days[0].system.map((s) => s.text),
    ["214 kayıt sunucuya gitti", "Gateway bağlantısı koptu (gün içinde 2 kez)"]);
  assert.equal(d.days[1].label, "Dün");
  assert.equal(d.days[1].system.length, 1);
});
