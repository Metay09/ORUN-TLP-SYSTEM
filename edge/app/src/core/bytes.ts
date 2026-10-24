// Small byte helpers. Every multi-byte field on the ORUN wire is big endian.

export function hex(b: Uint8Array): string {
  let s = "";
  for (const v of b) s += v.toString(16).padStart(2, "0");
  return s.toUpperCase();
}

export function fromHex(text: string): Uint8Array {
  if (text.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(text))
    throw new Error("bad hex");
  const out = new Uint8Array(text.length / 2);
  for (let i = 0; i < out.length; i++)
    out[i] = parseInt(text.slice(2 * i, 2 * i + 2), 16);
  return out;
}

export function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

export function equal(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i];
  return d === 0;
}

export function ascii(text: string): Uint8Array {
  const out = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0x7f;
  return out;
}

/** Right-pads with zeros to exactly n bytes (the CC310 fixed-size inputs). */
export function padTo(b: Uint8Array, n: number): Uint8Array {
  if (b.length > n) throw new Error("too long");
  const out = new Uint8Array(n);
  out.set(b);
  return out;
}

export function u16(b: Uint8Array, o: number): number {
  return (b[o] << 8) | b[o + 1];
}
export function i16(b: Uint8Array, o: number): number {
  const v = u16(b, o);
  return v & 0x8000 ? v - 0x10000 : v;
}
export function i8(b: Uint8Array, o: number): number {
  return b[o] & 0x80 ? b[o] - 0x100 : b[o];
}
export function u32(b: Uint8Array, o: number): number {
  return ((b[o] << 24) >>> 0) + (b[o + 1] << 16) + (b[o + 2] << 8) + b[o + 3];
}
export function i32(b: Uint8Array, o: number): number {
  return u32(b, o) | 0;
}
/** 64-bit values stay bigint: gateway ids and generations use all 64 bits. */
export function u64(b: Uint8Array, o: number): bigint {
  let v = 0n;
  for (let i = 0; i < 8; i++) v = (v << 8n) | BigInt(b[o + i]);
  return v;
}

export function be16(v: number): Uint8Array {
  return new Uint8Array([(v >> 8) & 0xff, v & 0xff]);
}
export function be64(v: bigint): Uint8Array {
  const out = new Uint8Array(8);
  for (let i = 7; i >= 0; i--) { out[i] = Number(v & 0xffn); v >>= 8n; }
  return out;
}

export function hex64(v: bigint): string {
  return v.toString(16).toUpperCase().padStart(16, "0");
}

export function isZero(b: Uint8Array): boolean {
  return b.every((v) => v === 0);
}
