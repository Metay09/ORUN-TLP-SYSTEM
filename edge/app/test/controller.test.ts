// Application sync orchestration: no Android hardware required.
import assert from "node:assert/strict";
import { test } from "node:test";
import { Controller } from "../src/app/controller.ts";
import type { UploadCounts, RunResult } from "../src/core/sync.ts";

const empty: UploadCounts = { uploaded: 0, pending: 0, conflict: 0, rejected: 0 };
const withPending = (n: number): UploadCounts => ({ ...empty, pending: n });

function prepared(): Controller {
  const c = new Controller();
  c.snap.server = {
    url: "http://backend.test:8090", token: "token", auto: true,
    busy: false, counts: null, lastOk: null, error: null,
  };
  return c;
}

function installUploader(c: Controller, uploader: {
  counts: () => Promise<UploadCounts>;
  drain: (url: string, token: string) => Promise<RunResult>;
  ping: (url: string, token: string) => Promise<number | null>;
}): void {
  // Inject the app's upload boundary without starting BLE, USB, or timers.
  (c as unknown as { uploader: typeof uploader }).uploader = uploader;
}

test("sync: read-key endpoint failure does not hold up already verified uploads", async () => {
  const c = prepared();
  let countCalls = 0;
  let uploads = 0;
  let keyAttempts = 0;
  installUploader(c, {
    counts: async () => ++countCalls === 1 ? withPending(2) : { ...empty, uploaded: 2 },
    drain: async (url, token) => {
      assert.deepEqual([url, token], ["http://backend.test:8090", "token"]);
      uploads++;
      return { sent: 2, final: 2, uncertain: 0 };
    },
    ping: async () => { throw new Error("should not ping"); },
  });
  (c as unknown as { refreshKeys: () => Promise<void> }).refreshKeys = async () => {
    keyAttempts++;
    throw new Error("keys temporarily unavailable");
  };

  await c.sync(true);
  assert.equal(keyAttempts, 1);
  assert.equal(uploads, 1);
  assert.equal(countCalls, 2);
  assert.equal(c.snap.server.busy, false);
  assert.equal(c.snap.server.counts?.uploaded, 2);
  assert.match(c.snap.server.error ?? "", /keys temporarily unavailable/);
});

test("sync: simultaneous timer and manual calls cannot overlap uploads", async () => {
  const c = prepared();
  // No key refresh needed in this test; observe only the uploader mutex.
  (c as unknown as { keysAt: number }).keysAt = Date.now();
  let release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  let countsCalls = 0;
  let uploads = 0;
  installUploader(c, {
    counts: async () => {
      countsCalls++;
      if (countsCalls === 1) await gate;
      return countsCalls === 1 ? withPending(1) : { ...empty, uploaded: 1 };
    },
    drain: async () => { uploads++; return { sent: 1, final: 1, uncertain: 0 }; },
    ping: async () => { throw new Error("should not ping"); },
  });

  const a = c.sync(false);
  const b = c.sync(true);
  release();
  await Promise.all([a, b]);
  assert.equal(uploads, 1);
  assert.equal(countsCalls, 2); // first pass before and after; second entry skipped
  assert.equal(c.snap.server.busy, false);
});
