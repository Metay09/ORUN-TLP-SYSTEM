// 4a: tasmanın canlı durumu USB'den ve Bluetooth'tan aynı okunur.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { LineConsole } from "../src/app/addTracker.ts";
import { AppReassembler, checkAnswer, requestFrame, type AppMessage, type Family } from "../src/app/bleAppFrames.ts";
import { buildStatus, decodeBle, keyValues, readRawUsb, serviceStateName, type Raw } from "../src/app/trackerStatus.ts";

// The lines firmware/src/usb_application_adapter.cpp prints for one state...
const USB_LINES: Record<Family, string> = {
  config: "APP RESULT id=1 code=OK config_backend_ready=yes source=stored tracking_interval_seconds=900 battery_capacity_mah=1000",
  device: "APP DEVICE id=2 fw=0.9.3-sf5l0 rev=2 uptime_ms_mod32=3600000 reset=0x00000001 watchdog=no role=0 role_mode=AUTO relay_applied=no gnss_presence=1 gnss_health=0 accel_presence=1 accel_health=0 tracking_state=1 tracking_reason=0 relay_state=0 relay_reason=1",
  tracking: "APP TRACKING id=3 requested_s=900 applied_s=900 effective_s=300 cadence=1 gnss_state=11 detected=yes extra_fix=no attempts=40 fresh=38 timeouts=2 invalid=0 last_ttff_ms=22000 config_ready=yes source=stored",
  geofence: "APP GEOFENCE id=4 resource=2 token=1 areas=2 vertices=9 runtime=configured confirmed=yes state=2 confirmation=idle cadence=1",
  storage: "APP STORAGE id=5 history_ready=yes busy=no count=672 capacity=1056 overwritten=0 append_failures=0 corrupt=0 metadata_failures=0 config_ready=yes config_maintenance=no config_corrupt=0 geofence_ready=yes geofence_maintenance=no geofence_corrupt=0 security_ready=yes security_state=1 security_exhausted=no security_corrupt=0",
  power: "APP POWER id=6 battery=MEASURED state=NORMAL percent_est=81 mv=3950 raw=2900 age_ms=1200 readings=50 calibrated=no",
  services: "APP SERVICES mode=EXPLICIT requested=T applied=EXPLICIT tracking=ON relay=OFF receive=OFF legacy_mode=TRACKER",
};

// ...and the bytes firmware/src/ble_application_transport.cpp sends for it.
const le = (n: number, size: number) => Array.from({ length: size }, (_, i) => (n >>> (8 * i)) & 0xff);
const fw = (t: string) => [...new TextEncoder().encode(t), ...new Array(16 - t.length).fill(0)];
const BLE: Record<Family, number[]> = {
  config: [0, 0x03, ...le(900, 4), ...le(1000, 4)],
  device: [0, 2, 0, 0x01, 1, 0, 1, 0, 1, 0, 0, 1, ...le(3600000, 4), ...le(1, 4), ...fw("0.9.3-sf5l0")],
  tracking: [0, 0x07, 1, 11, ...le(900, 4), ...le(900, 4), ...le(300, 4), ...le(40, 4), ...le(38, 4), ...le(2, 4), ...le(0, 4), ...le(22000, 4)],
  geofence: [0, 2, 1, 0x03, 2, ...le(9, 2), 2, 1],
  storage: [0, 0x55, 1, 0, ...le(672, 4), ...le(1056, 4), ...le(0, 4), ...le(0, 4), ...new Array(20).fill(0)],
  power: [0, 0x01, 1, 81, ...le(3950, 2), ...le(2900, 2), ...le(1200, 4), ...le(50, 4)],
  services: [0, 1, 0x01, 0x01, 1, 0, 0, 1, 0],
};
const TYPE: Record<Family, number> = { config: 1, device: 2, tracking: 3, geofence: 4, storage: 5, power: 6, services: 7 };
const msg = (f: Family): AppMessage => ({ type: TYPE[f] | 0x80, correlation: 7, payload: Uint8Array.from(BLE[f]) });

function frames(type: number, corr: number, payload: number[]): Uint8Array[] {
  const out: Uint8Array[] = [];
  for (let i = 0, k = 0; i < payload.length || k === 0; i += 12, k++) {
    const chunk = payload.slice(i, i + 12);
    const flags = (k === 0 ? 1 : 0) | (i + 12 >= payload.length ? 2 : 0);
    out.push(Uint8Array.from([1, type, flags, k, corr & 0xff, corr >> 8, payload.length & 0xff, payload.length >> 8, ...chunk]));
  }
  return out;
}

test("a query is one 8-byte frame with no data", () => {
  assert.deepEqual([...requestFrame(0x03, 0x1234)], [1, 3, 3, 0, 0x34, 0x12, 0, 0]);
});

test("a 36-byte answer arrives in three fragments", () => {
  const r = new AppReassembler();
  const fs = frames(0x82, 9, BLE.device);
  assert.equal(fs.length, 3);
  assert.equal(r.push(fs[0]), null);
  assert.equal(r.push(fs[1]), null);
  const m = r.push(fs[2])!;
  assert.deepEqual([m.type, m.correlation, m.payload.length], [0x82, 9, 36]);
});

test("broken fragments are dropped, errors are reported", () => {
  const r = new AppReassembler();
  const fs = frames(0x82, 9, BLE.device);
  r.push(fs[0]);
  assert.equal(r.push(fs[2]), null);                 // skipped one
  assert.equal(r.push(fs[1]), null);                 // nothing in progress any more
  const err = r.push(frames(0xff, 9, [2, 0x05])[0])!;
  assert.throws(() => checkAnswer(err, "storage"), /meşgul/);
  assert.throws(() => checkAnswer(msg("power"), "storage"), /beklenmedik/);
});

test("USB and Bluetooth give the same status", () => {
  const usb: Raw = {}, ble: Raw = {};
  for (const f of Object.keys(USB_LINES) as Family[]) { usb[f] = keyValues(USB_LINES[f]); ble[f] = decodeBle(f, msg(f)); }
  const a = buildStatus(usb, "usb", 1), b = buildStatus(ble, "ble", 1);
  assert.deepEqual({ ...b, via: "usb" }, a);
  assert.deepEqual(a.interval, { requestedS: 900, effectiveS: 300, stored: true });
  assert.deepEqual(a.battery, { valid: true, mv: 3950, percent: 81, state: "normal", calibrated: false });
  assert.deepEqual(a.fence, { state: "configured", areas: 2, vertices: 9, position: "outside" });
  assert.deepEqual(a.services, { explicit: true, requested: { tracking: true, relay: false, receive: false },
    tracking: "on", trackingWhy: null, relay: "off", receive: false });
  assert.deepEqual([a.role, a.firmware, a.gnss?.state, a.storage?.count, a.missing], ["tracker", "0.9.3-sf5l0", 11, 672, []]);
});

test("service words match the firmware's USB spelling", () => {
  assert.equal(serviceStateName(2, 5), "BLOCKED_GNSS_ABSENT");
  assert.equal(serviceStateName(2, 4), "BLOCKED_GNSS_PENDING");
  assert.equal(serviceStateName(2, 1), "BLOCKED");
  assert.equal(buildStatus({ services: keyValues("APP SERVICES mode=EXPLICIT requested=T applied=EXPLICIT tracking=BLOCKED_GNSS_ABSENT relay=OFF receive=OFF legacy_mode=TRACKER") }, "usb", 0)
    .services?.trackingWhy, "BLOCKED_GNSS_ABSENT");
});

test("USB: a family the device refuses is listed as missing", async () => {
  const out: string[] = [];
  const con: LineConsole = {
    async send(line) {
      const f = (Object.keys(USB_LINES) as Family[]).find((k) => ({ config: "APP CONFIG?" } as Record<string, string>)[k] === line
        || line === `APP ${k.toUpperCase()}?`);
      out.push("GNSS fix pending", f === "storage" ? "APP RESULT id=9 code=UNAVAILABLE" : USB_LINES[f!]);
    },
    async waitFor(match) { while (out.length) { const r = match(out.shift()!); if (r !== null) return r; } return null; },
  };
  const st = buildStatus(await readRawUsb(con), "usb", 0);
  assert.deepEqual(st.missing, ["storage"]);
  assert.equal(st.interval?.requestedS, 900);
});

// 4b: ayar satırları ve tasmanın yanıtı.
import { applySetting, intervalCommand, servicesCommand } from "../src/app/trackerSettings.ts";

function settingConsole(answer: string | null): LineConsole & { sent: string[] } {
  const out: string[] = [];
  return {
    sent: [] as string[],
    async send(line) { this.sent.push(line); out.push("GNSS fix pending"); if (answer) out.push(answer); },
    async waitFor(match) { while (out.length) { const r = match(out.shift()!); if (r !== null) return r; } return null; },
  };
}

test("setting lines use the firmware's USB spelling and limits", () => {
  assert.equal(intervalCommand(180), "APP INTERVAL 180");
  assert.throws(() => intervalCommand(59), /1 dakika/);
  assert.throws(() => intervalCommand(12 * 24 * 3600 + 1), /12 gün/);
  assert.throws(() => intervalCommand(90.5), /arasında/);
  assert.equal(servicesCommand({ tracking: true, relay: false }), "APP SERVICES T");
  assert.equal(servicesCommand({ tracking: true, relay: true }), "APP SERVICES TR");
  assert.equal(servicesCommand({ tracking: false, relay: true }), "APP SERVICES R");
  assert.equal(servicesCommand({ tracking: false, relay: false }), "APP SERVICES NONE");
});

test("a setting counts only when the tracker says APPLIED or UNCHANGED", async () => {
  assert.equal(await applySetting(settingConsole("APP SET id=4 code=APPLIED tracking_interval_seconds=300 token=VALID revision=9"), "APP INTERVAL 300"), "applied");
  assert.equal(await applySetting(settingConsole("APP SET id=5 code=UNCHANGED services=T mode=EXPLICIT token=VALID revision=9"), "APP SERVICES T"), "unchanged");
  await assert.rejects(applySetting(settingConsole("APP SET id=6 code=INVALID tracking_interval_seconds=180 token=VALID revision=9"), "APP INTERVAL 30"), /kabul etmedi/);
  await assert.rejects(applySetting(settingConsole("APP SET id=7 code=OUTCOME_UNKNOWN services=T mode=EXPLICIT token=UNAVAILABLE"), "APP SERVICES T"), /belli değil/);
  await assert.rejects(applySetting(settingConsole("APP BUSY"), "APP SERVICES T"), /meşgul/);
  await assert.rejects(applySetting(settingConsole(null), "APP SERVICES T"), /yanıt gelmedi/);
});
