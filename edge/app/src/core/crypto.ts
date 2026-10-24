// Crypto primitives, synchronous (audited @noble libraries).
// WebCrypto has no AES-CCM, so CCM (RFC 3610) is built here from AES-CBC
// (the CBC-MAC) and AES-CTR, both from @noble/ciphers. Cross-checked against
// Python `cryptography` AESCCM (test/core.test.ts, test_sf5l7_edge_app.py).
import { cbc, ctr } from "@noble/ciphers/aes.js";
import { hkdf } from "@noble/hashes/hkdf.js";
import { hmac } from "@noble/hashes/hmac.js";
import { sha256 } from "@noble/hashes/sha2.js";
import { concat, equal } from "./bytes.ts";

export { sha256 };

export function hkdfSha256(ikm: Uint8Array, salt: Uint8Array, info: Uint8Array,
                           length: number): Uint8Array {
  return hkdf(sha256, ikm, salt, info, length);
}

export function hmacSha256(key: Uint8Array, message: Uint8Array): Uint8Array {
  return hmac(sha256, key, message);
}

const pad16 = (b: Uint8Array) =>
  concat(b, new Uint8Array((16 - (b.length % 16)) % 16));

function counterBlock(nonce: Uint8Array, i: number): Uint8Array {
  const a = new Uint8Array(16);
  a[0] = 15 - nonce.length - 1;   // L - 1
  a.set(nonce, 1);
  a[15] = i;
  return a;
}

// CBC-MAC tag (before the S_0 mask) over B_0 | AAD | plaintext.
function cbcMac(key: Uint8Array, nonce: Uint8Array, tagLength: number,
                aad: Uint8Array, plaintext: Uint8Array): Uint8Array {
  const L = 15 - nonce.length;
  const b0 = new Uint8Array(16);
  b0[0] = (aad.length ? 0x40 : 0) | (((tagLength - 2) / 2) << 3) | (L - 1);
  b0.set(nonce, 1);
  let n = plaintext.length;
  for (let i = 15; i > 15 - L; i--) { b0[i] = n & 0xff; n = Math.floor(n / 256); }
  const aadBlock = aad.length
    ? pad16(concat(new Uint8Array([aad.length >> 8, aad.length & 0xff]), aad))
    : new Uint8Array(0);
  const chain = cbc(key, new Uint8Array(16), { disablePadding: true })
    .encrypt(concat(b0, aadBlock, pad16(plaintext)));
  return chain.slice(chain.length - 16, chain.length - 16 + tagLength);
}

function valid(nonce: Uint8Array, tagLength: number, length: number,
               aad: Uint8Array): boolean {
  const L = 15 - nonce.length;
  return nonce.length >= 7 && nonce.length <= 13 && tagLength >= 4 &&
    tagLength <= 16 && tagLength % 2 === 0 && aad.length < 0xff00 &&
    (L >= 4 || length < 2 ** (8 * L));
}

// CTR with A_1, A_2, ...: a 16-byte big-endian increment equals CCM's L-byte
// counter while it cannot overflow, which valid() guarantees.
function keystreamXor(key: Uint8Array, nonce: Uint8Array, data: Uint8Array) {
  return data.length ? ctr(key, counterBlock(nonce, 1)).encrypt(data)
                     : new Uint8Array(0);
}

function maskTag(key: Uint8Array, nonce: Uint8Array, t: Uint8Array) {
  const s0 = ctr(key, counterBlock(nonce, 0)).encrypt(new Uint8Array(16));
  return t.map((v, i) => v ^ s0[i]);
}

/** AES-CCM decrypt + verify. null on any authentication or size failure. */
export function aesCcmDecrypt(key: Uint8Array, nonce: Uint8Array,
                              sealed: Uint8Array, aad: Uint8Array,
                              tagLength: number): Uint8Array | null {
  if (sealed.length < tagLength ||
      !valid(nonce, tagLength, sealed.length - tagLength, aad))
    return null;
  const ctLen = sealed.length - tagLength;
  const plaintext = keystreamXor(key, nonce, sealed.subarray(0, ctLen));
  const expected = maskTag(key, nonce, cbcMac(key, nonce, tagLength, aad, plaintext));
  return equal(expected, sealed.subarray(ctLen)) ? plaintext : null;
}

/** AES-CCM encrypt (tests and the simulated Gateway only). */
export function aesCcmEncrypt(key: Uint8Array, nonce: Uint8Array,
                              plaintext: Uint8Array, aad: Uint8Array,
                              tagLength: number): Uint8Array {
  if (!valid(nonce, tagLength, plaintext.length, aad)) throw new Error("ccm size");
  return concat(keystreamXor(key, nonce, plaintext),
                maskTag(key, nonce, cbcMac(key, nonce, tagLength, aad, plaintext)));
}
