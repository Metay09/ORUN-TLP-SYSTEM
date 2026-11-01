// Uygulama düzeni: Bağlantı sekmesinin rengi ve tek ad kuralı.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { connectionTone } from "../src/features/network/words.ts";

const base = { gatewayConnected: false, signedIn: true, syncError: false, lastSyncAt: null, now: 1_000_000_000 };

test("Bağlantı rengi: gateway bağlı ya da yeni eşitleme yeşil, hiç veri yolu yoksa kırmızı", () => {
  assert.equal(connectionTone({ ...base, gatewayConnected: true, signedIn: false }), "ok");
  assert.equal(connectionTone({ ...base, lastSyncAt: base.now - 60_000 }), "ok");
  assert.equal(connectionTone({ ...base, lastSyncAt: base.now - 10 * 60_000 }), "wait");
  assert.equal(connectionTone(base), "wait");
  assert.equal(connectionTone({ ...base, signedIn: false }), "bad");
  assert.equal(connectionTone({ ...base, syncError: true, lastSyncAt: base.now - 1000 }), "bad");
});

test("ekranlarda tek ad: İstasyon değil Gateway, Mera değil Alan", () => {
  const text = readFileSync(new URL("../src/app/i18n/tr.ts", import.meta.url), "utf8");
  assert.doesNotMatch(text, /İstasyon|istasyon/);
  assert.doesNotMatch(text, /"Mera|mera sınır/);
});
