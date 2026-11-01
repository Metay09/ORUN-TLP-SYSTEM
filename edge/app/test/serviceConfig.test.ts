// The same account origin module is imported by browser code and bare Node
// --test suites; Vite injects import.meta.env only in the browser build.
import assert from "node:assert/strict";
import { test } from "node:test";
import { ORUN_ACCOUNT_ORIGIN } from "../src/app/orunServiceConfig.ts";

test("account origin loads under plain Node ESM without Vite env injection", () => {
  const origin = new URL(ORUN_ACCOUNT_ORIGIN);
  assert.equal(origin.protocol, "https:");
  assert.equal(origin.username, "");
  assert.equal(origin.password, "");
  assert.equal(origin.search, "");
  assert.equal(origin.hash, "");
  assert.equal(ORUN_ACCOUNT_ORIGIN, origin.origin);
});
