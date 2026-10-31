// Evidence boundary: PC Edge is independent of this Android phone.
// These tests forbid showing server delivery success based on login, ping or
// old upload counters when newer errors/pending records remain.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { ServerView } from "../src/app/model.ts";
import { phonePathWords, serverWords } from "../src/features/network/words.ts";

const server = (patch: Partial<ServerView> = {}): ServerView => ({
  configured: true, url: "", token: "", auto: true, busy: false,
  uploaded: null, pending: null, conflict: 0, rejected: 0, lastOk: null,
  error: null, ...patch,
});

test("no phone link does not mean PC Edge or gateway are offline", () => {
  const path = phonePathWords(
    { link: "none", phase: "idle" },
    { ready: true, records: 94 },
  );
  assert.equal(path.stationConnected, false);
  assert.match(path.stationText, /Bu telefon/);
  assert.match(path.phoneText, /94 yerel kayıt/);
  assert.equal(path.phoneTone, "fresh");
});

test("phone station link requires both live connection and usable ledger", () => {
  assert.equal(phonePathWords(
    { link: "usb", phase: "connected" },
    { ready: false, records: null },
  ).stationConnected, false);
  assert.equal(phonePathWords(
    { link: "usb", phase: "connected" },
    { ready: true, records: 0 },
  ).stationConnected, true);
  assert.equal(phonePathWords(
    { link: "ble", phase: "waiting" },
    { ready: true, records: 2 },
  ).stationConnected, false);
  assert.equal(phonePathWords(
    { link: "none", phase: "idle" },
    { ready: true, records: 0 },
  ).phoneTone, "aging");
});

test("account login alone cannot mark observations delivered", () => {
  assert.notEqual(serverWords(server()).tone, "ok");
  assert.notEqual(serverWords(server({ pending: 0 })).tone, "ok");
  assert.notEqual(serverWords(server({ pending: 0, lastOk: "01:11:38", uploaded: 0 })).tone, "ok");
  assert.equal(serverWords(server({ configured: false, pending: 0, uploaded: 44, lastOk: "01:11" })).tone, "none");
});

test("only confirmed phone uploads with no pending, reject or current error can be green", () => {
  const done = server({ pending: 0, uploaded: 11, lastOk: "01:11:38" });
  assert.equal(serverWords(done).tone, "ok");
  assert.match(serverWords(done).text, /Bu telefonun/);
  assert.notEqual(serverWords({ ...done, pending: 1 }).tone, "ok");
  assert.equal(serverWords({ ...done, error: "503" }).tone, "off");
  assert.equal(serverWords({ ...done, conflict: 1 }).tone, "off");
  assert.equal(serverWords({ ...done, rejected: 1 }).tone, "off");
  assert.equal(serverWords({ ...done, busy: true }).tone, "up");
});
