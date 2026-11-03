// 4b: cihaz ayarları hesapta; bekleyenler bağlanınca yazılır.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { LineConsole } from "../src/app/addTracker.ts";
import {
  bleWindowWord, DeviceSettingsBook, pendingKeys, settingRows, valuesFromStatus,
  type DeviceSettingsEntry, type SettingsServer, type SettingValues, type Via,
} from "../src/app/deviceSettings.ts";
import { bleWindowCommand, writeWaitingUsb } from "../src/app/trackerSettings.ts";
import { buildStatus, keyValues } from "../src/app/trackerStatus.ts";

const A = "0E8ADE7E71538B3C", B = "0E8ADE7E71531AA3";
const entry = (wanted: SettingValues, reported: SettingValues | null): DeviceSettingsEntry =>
  ({ wanted, wantedAt: 1, reported, reportedAt: reported ? 2 : null, reportedVia: reported ? "usb" : null });

test("a setting waits until the device reports the same value", () => {
  assert.deepEqual(pendingKeys(null), []);
  assert.deepEqual(pendingKeys(entry({ interval_s: 900 }, null)), ["interval_s"]);
  assert.deepEqual(pendingKeys(entry({ interval_s: 900, relay: true }, { interval_s: 900, relay: false, tracking: true })), ["relay"]);
  const rows = settingRows(entry({ interval_s: 900 }, { interval_s: 180, tracking: true }));
  assert.deepEqual(rows.map((r) => [r.def.key, r.value, r.onDevice, r.state]),
    [["interval_s", 900, 180, "waiting"], ["tracking", true, true, "device"], ["relay", null, null, "unknown"],
     ["ble_window_s", null, null, "unknown"]]);
});

test("4c-1a: Bluetooth after each report is one more setting", async () => {
  // USB: the config line carries it; BLE: two bytes appended (older firmware: absent).
  const usb = buildStatus({ config: keyValues("APP RESULT id=1 code=OK config_backend_ready=yes source=stored tracking_interval_seconds=180 battery_capacity_mah=0 ble_window_seconds=45") }, "usb", 0);
  assert.equal(usb.bleWindowS, 45);
  assert.deepEqual(valuesFromStatus(usb), { interval_s: 180, ble_window_s: 45 });
  const old = buildStatus({ config: keyValues("APP RESULT id=1 code=OK config_backend_ready=yes source=stored tracking_interval_seconds=180 battery_capacity_mah=0") }, "usb", 0);
  assert.equal(old.bleWindowS, null);
  assert.deepEqual(valuesFromStatus(old), { interval_s: 180 });
  assert.equal(bleWindowCommand(30), "APP BLE WINDOW 30");
  assert.equal(bleWindowCommand(0), "APP BLE WINDOW 0");
  assert.throws(() => bleWindowCommand(301));
  assert.throws(() => bleWindowCommand(1.5));
  assert.equal(bleWindowWord(0), "Kapalı");
  assert.equal(bleWindowWord(30), "30 sn");
  assert.equal(bleWindowWord(120), "2 dk");
  // Waiting value is written over USB with its own command.
  const t = fakeTracker();
  const done = await writeWaitingUsb(t, { ble_window_s: 60, interval_s: 180 }, { ble_window_s: 30, interval_s: 180 });
  assert.deepEqual(t.sent, ["APP BLE WINDOW 60"]);
  assert.deepEqual(done, { ble_window_s: 60 });
});

test("what a live read says the tracker has", () => {
  const st = buildStatus({
    tracking: keyValues("APP TRACKING id=3 requested_s=180 applied_s=180 effective_s=60 cadence=1 gnss_state=8 detected=yes extra_fix=no attempts=1 fresh=1 timeouts=0 invalid=0 last_ttff_ms=900 config_ready=yes source=stored"),
    services: keyValues("APP SERVICES mode=EXPLICIT requested=TR applied=EXPLICIT tracking=ON relay=ON receive=OFF legacy_mode=TRACKER"),
  }, "usb", 0);
  assert.deepEqual(valuesFromStatus(st), { interval_s: 180, tracking: true, relay: true });
  // AUTO: the firmware decides; no service value is claimed.
  const auto = buildStatus({ services: keyValues("APP SERVICES mode=AUTO requested=- applied=LEGACY_AUTO tracking=ON relay=OFF receive=OFF legacy_mode=TRACKER") }, "usb", 0);
  assert.deepEqual(valuesFromStatus(auto), {});
});

function fakeTracker(): LineConsole & { sent: string[] } {
  const out: string[] = [];
  return {
    sent: [] as string[],
    async send(line) { this.sent.push(line); out.push(`APP SET id=1 code=APPLIED ${line.startsWith("APP INTERVAL") ? "tracking_interval_seconds=" + line.slice(13) : "services=x mode=EXPLICIT"} token=VALID revision=2`); },
    async waitFor(match) { while (out.length) { const r = match(out.shift()!); if (r !== null) return r; } return null; },
  };
}

test("only what differs is written over USB, services as one line", async () => {
  let t = fakeTracker();
  assert.deepEqual(await writeWaitingUsb(t, { interval_s: 180 }, { interval_s: 180, tracking: true, relay: false }), {});
  assert.deepEqual(t.sent, []);
  t = fakeTracker();
  assert.deepEqual(await writeWaitingUsb(t, { interval_s: 900, relay: true }, { interval_s: 180, tracking: true, relay: false }),
    { interval_s: 900, tracking: true, relay: true });
  assert.deepEqual(t.sent, ["APP INTERVAL 900", "APP SERVICES TR"]);
  t = fakeTracker();
  await writeWaitingUsb(t, { tracking: false }, { interval_s: 180 });   // relay unknown: stays off
  assert.deepEqual(t.sent, ["APP SERVICES NONE"]);
});

function memStore() {
  const data = new Map<string, string>();
  return { data, getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => { data.set(k, v); } };
}

test("bulk and single choices wait on the phone until the account takes them", async () => {
  const store = memStore();
  const account: Record<string, DeviceSettingsEntry> = {};
  let online = false;
  const calls: string[] = [];
  const server: SettingsServer = {
    async deviceSettings() { if (!online) throw new Error("off"); return JSON.parse(JSON.stringify(account)); },
    async wantSettings(devices, set) {
      if (!online) throw new Error("off");
      calls.push("want " + devices.join(","));
      for (const d of devices) account[d] = { ...(account[d] ?? entry({}, null)), wanted: { ...(account[d]?.wanted ?? {}), ...set } };
    },
    async reportSettings(device, values, via: Via) {
      if (!online) throw new Error("off");
      calls.push("report " + device);
      account[device] = { ...(account[device] ?? entry({}, null)), reported: values, reportedVia: via };
    },
  };
  const book = new DeviceSettingsBook(store);
  await book.want([A.toLowerCase(), B], { interval_s: 900 }, server);   // offline
  await book.report(A, { interval_s: 180, tracking: true }, "usb", server);
  assert.deepEqual(pendingKeys(book.get(A)), ["interval_s"]);
  assert.equal(book.waiting(), 2);
  const again = new DeviceSettingsBook(store);                            // survives a restart
  assert.equal(again.get(B)?.wanted.interval_s, 900);
  online = true;
  await again.refresh(server);
  assert.deepEqual(calls, [`want ${A},${B}`, `report ${A}`]);   // in the order they were made
  assert.equal(again.waiting(), 0);
  assert.equal(again.get(A)?.reported?.interval_s, 180);
  assert.equal(again.get(B)?.wanted.interval_s, 900);
  await assert.rejects(again.want([], { relay: true }, server), /seçilmedi/);
  await assert.rejects(again.want([A], { interval_s: 30 }, server), /seçilmedi/);   // invalid value dropped
});

test("a never-read device shows what its own records say, marked as such", () => {
  const rows = settingRows(null, { interval_s: 900, tracking: true });
  assert.deepEqual(rows.map((r) => [r.def.key, r.value, r.state]),
    [["interval_s", 900, "observed"], ["tracking", true, "observed"], ["relay", null, "unknown"],
     ["ble_window_s", null, "unknown"]]);
  // A choice waits on top of what the records show; a direct read wins over records.
  assert.equal(settingRows(entry({ interval_s: 180 }, null), { interval_s: 900 })[0].state, "waiting");
  assert.equal(settingRows(entry({}, { interval_s: 180 }), { interval_s: 900 })[0].value, 180);
});
