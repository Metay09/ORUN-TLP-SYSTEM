// 1b: kalıcı giriş — e-postayla giriş, uzun oturum, saklanan oturumu geri alma.
import assert from "node:assert/strict";
import { test } from "node:test";
import { OwnerAccountClient } from "../src/app/ownerAccountClient.ts";

const ORIGIN = "https://orun.example";
const TOKEN = "T".repeat(64);
const INST = "account-" + "a".repeat(64);
const now = () => Math.floor(Date.now() / 1000);

function reply(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), {
    status, headers: { "content-type": "application/json" },
  });
}

test("e-posta ile giriş: sunucunun döndürdüğü kullanıcı adı ve 180 günlük süre kabul edilir", async () => {
  let sent: unknown = null;
  const client = new OwnerAccountClient(ORIGIN, (async (_url: string, init: RequestInit) => {
    sent = JSON.parse(String(init.body));
    return reply(200, {
      account_access_token: TOKEN, site: "orun-main", actor: "owner",
      username: "ownerlogin", installation: INST, expires_at: now() + 180 * 86400,
    });
  }) as typeof fetch);
  const user = await client.signInWithPassword(" Metay@Example.COM ", "x".repeat(16));
  assert.deepEqual(sent, { username: "metay@example.com", password: "x".repeat(16) });
  assert.equal(user.username, "ownerlogin");
  assert.ok(user.expiresAt > now() + 170 * 86400);
  assert.equal(client.storableSession()?.accessToken, TOKEN);
});

test("saklanan oturum geri alınır; sunucu reddederse kapanır, çevrimdışıysa açık kalır", async () => {
  const identity = { site: "orun-main", actor: "owner", username: "ownerlogin",
                     installation: INST, expiresAt: now() + 86400 };
  let answer: () => Promise<Response> = async () =>
    reply(200, { site: "orun-main", email: "metay@example.com", expires_at: now() + 180 * 86400 });
  const client = new OwnerAccountClient(ORIGIN, (async () => answer()) as typeof fetch);
  client.adopt(identity, TOKEN);
  assert.equal(client.current?.username, "ownerlogin");

  const ok = await client.refresh();
  assert.deepEqual(ok, { state: "ok", email: "metay@example.com" });
  assert.ok((client.current?.expiresAt ?? 0) > now() + 170 * 86400, "süre ileri kaydı");

  answer = async () => { throw new TypeError("network down"); };
  assert.equal((await client.refresh()).state, "offline");
  assert.ok(client.current, "internet yokken oturum kapanmaz");

  answer = async () => reply(401, { error: "unauthorized" });
  assert.equal((await client.refresh()).state, "expired");
  assert.equal(client.current, null);
  assert.throws(() => client.adopt({ ...identity, installation: "edge-x" }, TOKEN));
});
