// Charging and "full" from the collar's raw voltage, in the app (charge.ts).
// Owner decision 2026-11-16: USB and sun, last night's percent while the sun
// charges, and the sun's balance night to night.
import assert from "node:assert/strict";
import { test } from "node:test";
import { chargeOf, chargeTitle, learnFloat, type Reading } from "../src/app/charge.ts";
import { batteryPercent } from "../src/app/herd.ts";

const at = (d: number, h: number, m = 0) => new Date(2026, 10, d, h, m).getTime();
const usb = (t: number, mv: number): Reading => ({ t, mv, usb: true, power: "BATTERY" });
const bat = (t: number, mv: number): Reading => ({ t, mv, usb: false, power: "BATTERY" });

test("USB: climbing is charging, a peak then a drop is full (bench 4128→4175→4148)", () => {
  const rising = [usb(at(15, 20, 0), 4128), usb(at(15, 20, 10), 4150), usb(at(15, 20, 20), 4175)];
  assert.equal(chargeOf(rising, at(15, 20, 25)).state, "usb-charging");
  const full = [...rising, usb(at(15, 20, 30), 4148)];
  const c = chargeOf(full, at(15, 20, 35));
  assert.equal(c.state, "usb-full");
  assert.equal(c.floatMv, 4175);
  assert.equal(chargeTitle(c.state), "USB'de · doldu");
});

test("USB: one reading, or a flat charger voltage (no cell), says only USB'de", () => {
  assert.equal(chargeOf([usb(at(15, 20), 4150)], at(15, 20, 5)).state, "usb");
  const flat = [4255, 4256, 4254, 4255].map((mv, i) => usb(at(15, 20, i * 10), mv));
  assert.equal(chargeOf(flat, at(15, 21)).state, "usb");
});

test("USB: unplugged before the end is not full; a low peak is not a charge end", () => {
  const cut = [usb(at(15, 20, 0), 3900), usb(at(15, 20, 10), 3990), bat(at(15, 20, 20), 3880)];
  assert.equal(learnFloat(cut), null);
  const low = [usb(at(15, 20, 0), 3950), usb(at(15, 20, 10), 4000), usb(at(15, 20, 20), 3980)];
  assert.equal(chargeOf(low, at(15, 20, 25)).state, "usb-charging");
});

test("Sun: a climb off USB is charging; the percent shown is last night's", () => {
  const rs = [bat(at(16, 4, 0), 3900), bat(at(16, 9, 50), 3898), bat(at(16, 10, 0), 3905),
    bat(at(16, 10, 10), 3930), bat(at(16, 10, 20), 3950)];
  const c = chargeOf(rs, at(16, 10, 25));
  assert.equal(c.state, "sun-charging");
  assert.deepEqual(c.night, { t: at(16, 4), mv: 3900, percent: batteryPercent(3900) });
});

test("Sun: a cloud after a climb is not 'full' without this collar's learnt charge end", () => {
  const rs = [bat(at(16, 10, 0), 3900), bat(at(16, 10, 10), 3960), bat(at(16, 10, 20), 3990), bat(at(16, 10, 30), 3955)];
  assert.equal(chargeOf(rs, at(16, 10, 35)).state, "battery");
});

test("Sun: reaching the charge end learnt on USB, then dropping, is full", () => {
  const onUsb = [usb(at(14, 20, 0), 4128), usb(at(14, 20, 10), 4175), usb(at(14, 20, 20), 4148)];
  const sun = [bat(at(16, 11, 0), 4100), bat(at(16, 11, 10), 4140), bat(at(16, 11, 20), 4168), bat(at(16, 11, 30), 4150)];
  assert.equal(chargeOf([...onUsb, ...sun], at(16, 11, 35)).state, "sun-full");
  assert.equal(chargeOf(sun, at(16, 11, 35)).state, "battery", "no learnt charge end: never 'full' in the sun");
});

test("Sun balance: night against the night before, only if the sun charged in between", () => {
  const day = [bat(at(15, 10, 0), 3880), bat(at(15, 10, 10), 3905), bat(at(15, 10, 20), 3930), bat(at(15, 18, 0), 3920)];
  const rs = [bat(at(15, 4, 0), 3890), ...day, bat(at(16, 4, 0), 3920)];
  const c = chargeOf(rs, at(16, 8));
  assert.ok(c.solar);
  assert.equal(c.solar.deltaPct, batteryPercent(3920)! - batteryPercent(3890)!);
  assert.ok(c.solar.deltaPct > 0);
  // No climb during the day: plain drain, nothing to say about the sun.
  const dull = [bat(at(15, 4, 0), 3890), bat(at(15, 12, 0), 3885), bat(at(16, 4, 0), 3880)];
  assert.equal(chargeOf(dull, at(16, 8)).solar, null);
  // A USB charge in between: not the sun's doing.
  const plugged = [bat(at(15, 4, 0), 3890), ...day, usb(at(15, 20, 0), 4100), bat(at(16, 4, 0), 4050)];
  assert.equal(chargeOf(plugged, at(16, 8)).solar, null);
});

test("Night reading: the last one before dawn, not older than 30 hours", () => {
  const rs = [bat(at(14, 3, 0), 3950), bat(at(14, 4, 50), 3940), bat(at(14, 12, 0), 3930)];
  assert.equal(chargeOf(rs, at(14, 13)).night?.mv, 3940);
  assert.equal(chargeOf(rs, at(15, 12)).night, null);
});
