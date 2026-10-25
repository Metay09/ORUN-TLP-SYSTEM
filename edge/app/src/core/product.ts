// SF5B PRODUCT_SECURE frame: authenticate, decrypt, check canonical content.
// Port of backend/candidates/product_secure_verified_ledger.py
// (open_product_frame, _periodic_valid, _event_valid) and of
// tools/orun_edge_bench.py describe_periodic. Same inputs -> same result;
// test_sf5l7_edge_app.py compares the two on generated frames.
//
// BENCH: the phone opens frames only with the PUBLIC SF5L0 test credential.
// Production Edge credential handling is step 3 (real security).
import { aesCcmDecrypt, hkdfSha256 } from "./crypto.ts";
import { ascii, concat, hex, i32, u16, u32, u64 } from "./bytes.ts";

// Either a full root credential (bench test key; backend side) or, on the
// owner's phone (owner decision D1), only the per-epoch D2A traffic key.
export type Credential =
  | { credentialId: Uint8Array; root: Uint8Array }
  | { trafficKey: Uint8Array };
export type CredentialLookup = (device: Uint8Array, epoch: number) => Credential | null;

export const TEST_CREDENTIAL_ID = new Uint8Array(16).map((_, i) => 0xa0 + i);
export const TEST_ROOT = new Uint8Array(32).map((_, i) => i);
export const TEST_KEY_EPOCH = 0x01020304;

export const benchCredentials: CredentialLookup = (_device, epoch) =>
  epoch === TEST_KEY_EPOCH ? { credentialId: TEST_CREDENTIAL_ID, root: TEST_ROOT } : null;

export interface Observation {
  deviceId: Uint8Array;      // authenticated 8-byte header identity
  incarnation: Uint8Array;   // authenticated 8-byte logical record identity
  sequence: number;
  family: number;            // 1 PERIODIC, 2 EVENT
  plaintext: Uint8Array;
  frame: Uint8Array;
}

export class Reject extends Error {}

const okLocation = (source: number, lat: number, lon: number) =>
  source >= 1 && source <= 4 && lat >= -900000000 && lat <= 900000000 &&
  lon >= -1800000000 && lon <= 1800000000;

export function periodicValid(p: Uint8Array): boolean {
  if (p.length !== 64 || p[0] !== 1 || p[2] > 4 || p[3] > 3 || p[41] > 2 || p[42] > 2)
    return false;
  const flags = p[1], source = p[2], batteryState = p[3];
  if (u64(p, 4) === 0n || u32(p, 12) === 0) return false;
  const end = u32(p, 16), duration = u32(p, 20);
  if (!duration || Boolean(flags & 1) !== Boolean(end)) return false;
  const lat = i32(p, 24), lon = i32(p, 28), alt = i32(p, 32);
  const hdop = u16(p, 36), age = u16(p, 38), sats = p[40];
  const loc = Boolean(flags & 2), altitude = Boolean(flags & 4), ageValid = Boolean(flags & 8);
  if (!loc) {
    if (source || altitude || ageValid || lat || lon || alt || hdop || sats || age !== 65535)
      return false;
  } else {
    if (!okLocation(source, lat, lon) || (!altitude && alt)) return false;
    if ((ageValid && age > 65534) || (!ageValid && age !== 65535)) return false;
    if (source !== 1 && (hdop || sats)) return false;
  }
  const covered = u32(p, 44), active = u32(p, 48);
  if (!(flags & 16)) {
    if (covered || active || u16(p, 52) || u16(p, 54) || p[41]) return false;
  } else {
    if (!covered || active > covered || covered > duration) return false;
    if (p[41] !== (covered === duration ? 2 : 1)) return false;
  }
  if (flags & 128) {
    if (!(flags & 16) || p[43] > 200 || (!active && p[43])) return false;
  } else if (p[43]) {
    return false;
  }
  if (!(flags & 32) && (u16(p, 56) || batteryState || p[42])) return false;
  if (u16(p, 58) & ~0x07ff) return false;
  if (!(flags & 64) && (u16(p, 58) || p.subarray(60, 64).some((v) => v)))
    return false;
  return true;
}

export function eventValid(p: Uint8Array): boolean {
  if (p.length !== 48 || p[0] !== 1) return false;
  const [family, lifecycle, severity, flags, reason, source, context] = p.subarray(1, 8);
  if (!(family >= 1 && family <= 4 && lifecycle >= 1 && lifecycle <= 2 &&
        severity >= 1 && severity <= 3 && flags <= 15 && source <= 4 && context <= 2))
    return false;
  if ((family === 3 && reason !== 1 && reason !== 2) || (family !== 3 && reason !== 0))
    return false;
  if (u64(p, 8) === 0n || u32(p, 16) === 0 || u64(p, 24) === 0n) return false;
  if (Boolean(flags & 1) !== Boolean(u32(p, 20))) return false;
  const lat = i32(p, 32), lon = i32(p, 36), age = u16(p, 40);
  if (!(flags & 2)) {
    if (source || (flags & 4) || lat || lon || age !== 65535) return false;
  } else {
    if (!okLocation(source, lat, lon)) return false;
    if ((flags & 4 && age > 65534) || (!(flags & 4) && age !== 65535)) return false;
  }
  if (u16(p, 42)) return false;
  if (flags & 8) {
    if (context === 0) return false;
  } else if (context || u32(p, 44)) {
    return false;
  }
  return true;
}

/** Authenticates the whole frame before trusting any identity in it. */
export function openProductFrame(raw: Uint8Array, lookup: CredentialLookup): Observation {
  if (raw.length !== 84 && raw.length !== 100) throw new Reject("unsupported length");
  const header = raw.subarray(0, 28);
  const shape = `${header[3]},${header[6]},${raw.length}`;
  if (header[0] !== 2 || header[1] !== 4 || header[2] !== 2 || header[7] !== 0 ||
      header[5] !== 1 || (header[4] !== 2 && header[4] !== 3) ||
      (shape !== "1,64,100" && shape !== "2,48,84"))
    throw new Reject("invalid protected header");
  const device = header.slice(8, 16);
  const epoch = u32(header, 16);
  if (epoch === 0xffffffff || u64(header, 20) === 0n) throw new Reject("invalid epoch/counter");
  const credential = lookup(device, epoch);
  if (!credential) throw new Reject("unknown device/epoch");
  const key = "trafficKey" in credential
    ? credential.trafficKey
    : hkdfSha256(credential.root, credential.credentialId,
                 concat(ascii("ORUN-TLP-V2-AEAD"), new Uint8Array([1]), header.subarray(16, 20)), 16);
  if (key.length !== 16) throw new Reject("bad key");
  const nonce = concat(header.subarray(16, 20), new Uint8Array([1]), header.subarray(20, 28));
  const plaintext = aesCcmDecrypt(key, nonce, raw.subarray(28), header, 8);
  if (!plaintext) throw new Reject("AEAD authentication failed");
  const family = header[3];
  if (!(family === 1 ? periodicValid(plaintext) : eventValid(plaintext)))
    throw new Reject("noncanonical authenticated product data");
  const inc = family === 1 ? 4 : 8, seq = family === 1 ? 12 : 16;
  return {
    deviceId: device, incarnation: plaintext.slice(inc, inc + 8),
    sequence: u32(plaintext, seq), family, plaintext, frame: raw.slice(),
  };
}

const SOURCES = ["UNKNOWN", "GNSS", "FIXED", "PHONE", "MANUAL"];
const BATTERY = ["UNKNOWN", "NORMAL", "LOW", "CRITICAL"];

/** Readable fields of a canonical PERIODIC plaintext (describe_periodic). */
export interface Periodic {
  tracker: string; incarnation: string; sequence: number; period_s: number;
  time: string | null; lat: number | null; lon: number | null;
  sats: number | null; hdop: number | null; fix_age_s: number | null;
  battery_mv: number | null; battery: string | null;
  active_s: number | null; covered_s: number | null; source: string;
}

export function isoSeconds(epochSeconds: number): string {
  return new Date(epochSeconds * 1000).toISOString().replace(".000Z", "Z");
}

export function describePeriodic(device: Uint8Array, p: Uint8Array): Periodic {
  const flags = p[1];
  const out: Periodic = {
    tracker: hex(device), incarnation: hex(p.subarray(4, 12)),
    sequence: u32(p, 12), period_s: u32(p, 20),
    time: null, lat: null, lon: null, sats: null, hdop: null, fix_age_s: null,
    battery_mv: null, battery: null, active_s: null, covered_s: null,
    source: SOURCES[p[2]] ?? "?",
  };
  if (flags & 1) out.time = isoSeconds(u32(p, 16));
  if (flags & 2) {
    out.lat = i32(p, 24) / 1e7;
    out.lon = i32(p, 28) / 1e7;
    if (p[2] === 1) { out.sats = p[40]; out.hdop = u16(p, 36) / 100; }
    if (flags & 8) out.fix_age_s = u16(p, 38);
  }
  if (flags & 16) { out.covered_s = u32(p, 44); out.active_s = u32(p, 48); }
  if (flags & 32) { out.battery_mv = u16(p, 56); out.battery = BATTERY[p[3]] ?? "?"; }
  return out;
}

/** One-line Turkish summary, as the PC tool prints it. */
export function human(d: Periodic): string {
  const parts = [`tasma=${d.tracker}`, `seq=${d.sequence}`, `zaman=${d.time ?? "bilinmiyor"}`];
  if (d.lat !== null && d.lon !== null) {
    const q: string[] = [];
    if (d.sats !== null) q.push(`uydu=${d.sats}`);
    if (d.hdop) q.push(`hdop=${d.hdop.toFixed(2)}`);
    parts.push(`konum=${d.lat.toFixed(6)},${d.lon.toFixed(6)}` + (q.length ? ` (${q.join(" ")})` : ""));
  } else {
    parts.push("konum=YOK");
  }
  if (d.battery_mv !== null) parts.push(`pil=${d.battery_mv}mV ${d.battery}`);
  if (d.active_s !== null) parts.push(`aktif=${d.active_s}/${d.covered_s}s`);
  return parts.join(" ");
}
