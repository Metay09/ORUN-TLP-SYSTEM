// 4c: Adafruit nRF52 RAK4630 serial DFU (legacy HCI over SLIP).
// Distinct from Nordic Secure DFU object opcodes. No USB access in this file.
// A transport must bind to a previously identified device. Completion here
// means packets were ACKed, NOT that its firmware/keys/records were verified.
import type { FirmwarePackage } from "./firmwarePackage.ts";

const END = 0xc0, ESC = 0xdb, ESC_END = 0xdc, ESC_ESC = 0xdd;
const TYPE = 14, MAX_FRAME = 1100, CHUNK = 512;
const MAX_APP = 0x0c5000 - 0x026000; // ORUN reserved ProductStore starts at 0x0c5000

export interface DfuSerial { write(frame: Uint8Array): Promise<void>; read(timeoutMs: number): Promise<Uint8Array>; }
export interface DfuProgress { stage: "init" | "erase" | "transfer" | "activate"; done: number; total: number; }
export type ProgressFn = (progress: DfuProgress) => void;

export class DfuError extends Error {
  readonly uncertain: boolean;
  constructor(message: string, uncertain: boolean) { super(message); this.uncertain = uncertain; }
}

/** Matches Adafruit_nRF52_nrfutil crc16.calc_crc16, init 0xFFFF. */
export function crc16(input: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of input) {
    crc = ((crc >>> 8) & 0xff) | ((crc << 8) & 0xff00);
    crc ^= byte;
    crc ^= (crc & 0xff) >>> 4;
    crc ^= (crc << 8) << 4;
    crc ^= ((crc & 0xff) << 4) << 1;
  }
  return crc & 0xffff;
}

function le32(n: number): number[] {
  return [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
}
function cmd(op: number, ...values: number[]): Uint8Array {
  return Uint8Array.from([...le32(op), ...values]);
}

/** HCI header + CRC16-LE, escaped and delimited by 0xC0. Sequence 1..7,0. */
export function encodeHci(payload: Uint8Array, sequence: number): Uint8Array {
  if (sequence < 0 || sequence > 7 || !Number.isInteger(sequence) || payload.length > 1023)
    throw new Error("HCI dizisi veya paket boyutu geçersiz.");
  const h0 = sequence | (((sequence + 1) & 7) << 3) | 0xc0;
  const h1 = TYPE | ((payload.length & 15) << 4);
  const h2 = (payload.length >>> 4) & 255;
  const h3 = (-h0 - h1 - h2) & 255;
  const raw = Uint8Array.from([h0, h1, h2, h3, ...payload]);
  const crc = crc16(raw);
  const escaped: number[] = [END];
  for (const b of [...raw, crc & 255, (crc >>> 8) & 255]) {
    if (b === END) escaped.push(ESC, ESC_END);
    else if (b === ESC) escaped.push(ESC, ESC_ESC);
    else escaped.push(b);
  }
  escaped.push(END);
  return Uint8Array.from(escaped);
}

export interface HciAck { nextSequence: number; sequence: number; }
/** Decode and verify one complete escaped HCI ACK packet. */
export function decodeHciAck(frame: Uint8Array): HciAck {
  if (frame.length < 8 || frame[0] !== END || frame[frame.length - 1] !== END)
    throw new Error("HCI ACK çerçevesi tamamlanmamış.");
  const bytes: number[] = [];
  for (let i = 1; i < frame.length - 1; i++) {
    const b = frame[i];
    if (b === ESC) {
      const next = frame[++i];
      if (next !== ESC_END && next !== ESC_ESC) throw new Error("HCI kaçış dizisi bozuk.");
      bytes.push(next === ESC_END ? END : ESC);
    } else if (b === END) throw new Error("HCI iç içe çerçeve geçersiz.");
    else bytes.push(b);
  }
  if (bytes.length !== 6 || (bytes[1] & 15) !== TYPE || ((bytes[1] >>> 4) | (bytes[2] << 4)) !== 0 ||
      ((bytes[0] + bytes[1] + bytes[2] + bytes[3]) & 255) !== 0 ||
      (bytes[0] & 0xc0) !== 0xc0 ||
      crc16(Uint8Array.from(bytes.slice(0, 4))) !== (bytes[4] | (bytes[5] << 8)))
    throw new Error("HCI ACK tip/uzunluk/CRC doğrulanamadı.");
  return { nextSequence: (bytes[0] >>> 3) & 7, sequence: bytes[0] & 7 };
}

/** Streaming SLIP splitter, robust to partial reads, noise and two frames/read. */
export class SlipFrames {
  private started = false;
  private bytes: number[] = [];
  push(chunk: Uint8Array): Uint8Array[] {
    const result: Uint8Array[] = [];
    for (const b of chunk) {
      if (b === END) {
        if (this.started && this.bytes.length) result.push(Uint8Array.from([END, ...this.bytes, END]));
        this.started = true;
        this.bytes = [];
      } else if (this.started) {
        this.bytes.push(b);
        if (this.bytes.length > MAX_FRAME) { this.started = false; this.bytes = []; throw new Error("HCI cevap çerçevesi fazla uzun."); }
      }
    }
    return result;
  }
}

export interface DfuPacket { stage: DfuProgress["stage"]; data: Uint8Array; done: number; total: number; }
/** Deterministic application-only plan: START(APP=4), INIT, DATA (512 B), STOP. */
export function* dfuPlan(image: Uint8Array, init: Uint8Array): Generator<DfuPacket> {
  if (!image.length || image.length > MAX_APP || !init.length || init.length > 512)
    throw new Error("DFU uygulama veya init boyutu geçersiz.");
  const total = image.length;
  yield { stage: "init", data: cmd(3, ...le32(4), ...le32(0), ...le32(0), ...le32(total)), done: 0, total };
  yield { stage: "init", data: cmd(1, ...init, 0, 0), done: 0, total };
  for (let offset = 0; offset < image.length; offset += CHUNK) {
    const end = Math.min(offset + CHUNK, image.length);
    yield { stage: "transfer", data: cmd(4, ...image.subarray(offset, end)), done: end, total };
  }
  yield { stage: "activate", data: cmd(5), done: total, total };
}

const wait = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

/**
 * Send the verified application-only image over an already-matched serial DFU
 * bootloader link. No automatic retries: timeout after mutating command is
 * INDETERMINATE. Caller must run the PC-equivalent post-update readback.
 */
export async function transferDfu(link: DfuSerial, pkg: FirmwarePackage, progress: ProgressFn = () => {}): Promise<void> {
  let changed = false, seq = 0, sentData = 0;
  const reader = new SlipFrames();
  for (const packet of dfuPlan(pkg.image, pkg.initPacket)) {
    seq = (seq + 1) & 7;
    const frame = encodeHci(packet.data, seq);
    try {
      changed = true; // even the first write may cause flash erasure
      await link.write(frame);
      let accepted = false;
      const deadline = Date.now() + 3000;
      while (!accepted) {
        const remaining = deadline - Date.now();
        if (remaining <= 0) throw new Error("HCI onayı zaman aşımına uğradı.");
        const bytes = await link.read(remaining);
        for (const ackFrame of reader.push(bytes)) {
          const ack = decodeHciAck(ackFrame);
          if (ack.nextSequence === ((seq + 1) & 7)) { accepted = true; break; }
          throw new Error("HCI cihaz onayı beklenen diziyle uyuşmadı.");
        }
      }
    } catch (e) {
      throw new DfuError("DFU aktarım sonucu belirsiz: " + (e instanceof Error ? e.message : String(e)), changed);
    }
    progress({ stage: packet.stage, done: packet.done, total: packet.total });
    if (packet.stage === "init" && packet.data[0] === 3) {
      // Adafruit nRF52 bootloader erases flash pages after START.
      progress({ stage: "erase", done: 0, total: packet.total });
      await wait(Math.max(500, Math.ceil(pkg.image.length / 4096) * 90));
    }
    if (packet.stage === "transfer" && ++sentData % 8 === 0) await wait(105);
  }
  // STOP ACK merely means accepted by bootloader; post-flash readback is needed.
}
