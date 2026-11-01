// Remote account feed must stay separate from the local Gateway ledger.
// These tests do not touch Android, PC Edge, or backend state.
import assert from "node:assert/strict";
import { test } from "node:test";
import { OwnerAccountClient, type AccountIdentity } from "../src/app/ownerAccountClient.ts";
import { parseServerObservationPage } from "../src/app/serverObservationRead.ts";

const RECORD = {
  cursor: 1, device: "52E16773141A8887", incarnation: "5523010CEA37319F",
  sequence: 99, family: 1, frame: "AA".repeat(100),
  first_seen_utc: 1793495511, rssi: -9, snr: 8,
} as const;

test("server records must be bounded, ordered and sealed; malformed metadata never advances cursor", () => {
  const page = { records: [RECORD], cursor: 1, has_more: false };
  assert.equal(parseServerObservationPage(page, 0).records[0].device, RECORD.device);
  assert.deepEqual(parseServerObservationPage({ records: [], cursor: 1, has_more: false }, 1).records, []);
  const invalid = [
    { ...page, cursor: 2 },
    { ...page, has_more: true, records: [] },
    { ...page, records: [{ ...RECORD, cursor: 0 }] },
    { ...page, records: [{ ...RECORD, sequence: -1 }] },
    { ...page, records: [{ ...RECORD, frame: "00".repeat(84) }] },
    { ...page, records: [{ ...RECORD, device: "OTHER" }] },
    { ...page, records: [{ ...RECORD, rssi: 990 }] },
    { ...page, records: [RECORD, RECORD], cursor: 2 },
    { ...page, records: Array.from({ length: 65 }, (_, i) => ({ ...RECORD, cursor: i + 1 })), cursor: 65 },
  ];
  for (const value of invalid)
    assert.throws(() => parseServerObservationPage(value, 0));
});

test("account observation read uses the account bearer and never the legacy backend token", async () => {
  const seen: Array<{ url: string; options: RequestInit | undefined }> = [];
  const fetcher: typeof fetch = async (uri, options) => {
    seen.push({ url: String(uri), options });
    return new Response(JSON.stringify({ records: [RECORD], cursor: 1, has_more: false }), {
      headers: { "content-type": "application/json" }, status: 200,
    });
  };
  const client = new OwnerAccountClient("https://orun.example", fetcher);
  const identity: AccountIdentity = {
    site: "orun-main", actor: "owner", username: "owner",
    installation: "account-x", expiresAt: Date.now() / 1000 + 3600,
  };
  Object.assign(client, { accessToken: "account-test-bearer", identity });
  const out = await client.readServerObservations(0);
  assert.equal(out.cursor, 1);
  assert.equal(seen.length, 1);
  assert.equal(seen[0].url, "https://orun.example/v1/edge/records?after=0");
  const headers = seen[0].options?.headers as Record<string, string>;
  assert.equal(headers.Authorization, "Bearer account-test-bearer");
  assert.equal(seen[0].options?.redirect, "error");
  assert.equal(seen[0].options?.cache, "no-store");
});

test("a response arriving after account logout must not become a visible record", async () => {
  let unlock!: (value: Response) => void;
  const deferred = new Promise<Response>((resolve) => { unlock = resolve; });
  const fetcher: typeof fetch = async () => deferred;
  const client = new OwnerAccountClient("https://orun.example", fetcher);
  Object.assign(client, {
    accessToken: "account-test-bearer",
    identity: {
      site: "orun-main", actor: "owner", username: "owner",
      installation: "account-x", expiresAt: Date.now() / 1000 + 3600,
    },
  });
  const request = client.readServerObservations(0);
  Object.assign(client, { epoch: 1, accessToken: null, identity: null });
  unlock(new Response(JSON.stringify({ records: [RECORD], cursor: 1, has_more: false }), {
    headers: { "content-type": "application/json" }, status: 200,
  }));
  await assert.rejects(request, /hesap değişti/);
});

test("no account or expired account refuses before network", async () => {
  let calls = 0;
  const client = new OwnerAccountClient("https://orun.example", (async () => {
    calls++;
    throw new Error("network must stay closed");
  }) as typeof fetch);
  await assert.rejects(client.readServerObservations(0), /giriş/);
  Object.assign(client, { accessToken: "old", identity: {
    site: "orun-main", actor: "owner", username: "owner",
    installation: "account-x", expiresAt: 1,
  } });
  await assert.rejects(client.readServerObservations(0), /giriş/);
  assert.equal(calls, 0);
});
