// 4c: Read-only ORUN firmware package verification before any USB DFU operation.
// An ORUN package contains only application.bin/.dat and orun-package.json.
// No bootloader, SoftDevice, key, or configuration flash region may be included.
// This module DOES NOT authorize or perform a firmware update.

const MAX_ZIP = 2 * 1024 * 1024;
const MAX_BIN = 0x0c5000 - 0x026000; // audited application/storage boundary
const MAX_DAT = 512; // HCI init packet uses a single frame
const MAX_META = 16 * 1024;
const UTF8 = new TextDecoder("utf-8", { fatal: true });
const U16 = (d: DataView, at: number) => d.getUint16(at, true);
const U32 = (d: DataView, at: number) => d.getUint32(at, true);
const SIZES: Record<string, number> = {
  "orun-package.json": MAX_META, "application.bin": MAX_BIN, "application.dat": MAX_DAT,
};

export interface FirmwarePackage {
  sourceCommit: string;
  target: "RAK4630";
  image: Uint8Array;
  initPacket: Uint8Array;
  imageSha256: string;
  initSha256: string;
}

function reject(message: string): never { throw new Error("Yazılım paketi geçersiz: " + message); }
function hex(data: Uint8Array): string {
  return Array.from(data, (b) => b.toString(16).padStart(2, "0")).join("");
}
async function sha256(data: Uint8Array): Promise<string> {
  const copy = new Uint8Array(data).buffer;
  return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", copy)));
}
function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (const b of data) {
    crc ^= b;
    for (let i = 0; i < 8; i++) crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/** Central-directory-based bounded ZIP reader, only for this exact 3-file format. */
async function filesOfZip(b: Uint8Array): Promise<Record<string, Uint8Array>> {
  if (b.length < 22 || b.length > MAX_ZIP) reject("dosya boyutu izin verilen sınırda değil.");
  const d = new DataView(b.buffer, b.byteOffset, b.byteLength);
  // Python zipfile emits no archive comment. Reject appended data and zip64.
  const end = b.length - 22;
  if (U32(d, end) !== 0x06054b50 || U16(d, end + 20) !== 0 ||
      U16(d, end + 4) !== 0 || U16(d, end + 6) !== 0 ||
      U16(d, end + 8) !== 3 || U16(d, end + 10) !== 3)
    reject("ZIP dizini veya dosya sayısı geçersiz.");
  const cdSize = U32(d, end + 12), cdStart = U32(d, end + 16);
  if (cdStart + cdSize !== end) reject("ZIP dizin konumu geçersiz.");
  let at = cdStart;
  const files: Record<string, Uint8Array> = {};
  for (let n = 0; n < 3; n++) {
    if (at + 46 > end || U32(d, at) !== 0x02014b50) reject("ZIP girdisi bozuk.");
    const flags = U16(d, at + 8), method = U16(d, at + 10);
    const crc = U32(d, at + 16), compressedSize = U32(d, at + 20), plainSize = U32(d, at + 24);
    const nameLen = U16(d, at + 28), extraLen = U16(d, at + 30), commentLen = U16(d, at + 32);
    const localAt = U32(d, at + 42);
    const next = at + 46 + nameLen + extraLen + commentLen;
    if (next > end || nameLen > 64 || extraLen || commentLen || flags & 1 || ![0, 8].includes(method))
      reject("Desteklenmeyen ZIP girdisi.");
    const name = UTF8.decode(b.subarray(at + 46, at + 46 + nameLen));
    if (!Object.hasOwn(SIZES, name) || Object.hasOwn(files, name) || compressedSize > MAX_ZIP || plainSize > SIZES[name] || plainSize === 0)
      reject("Pakette beklenmeyen veya fazla büyük dosya var.");
    if (localAt + 30 > cdStart || U32(d, localAt) !== 0x04034b50 ||
        U16(d, localAt + 8) !== method || U16(d, localAt + 6) !== flags ||
        U32(d, localAt + 14) !== crc || U32(d, localAt + 18) !== compressedSize ||
        U32(d, localAt + 22) !== plainSize)
      reject("ZIP dosya başlığı uyuşmuyor.");
    const localNameLen = U16(d, localAt + 26), localExtraLen = U16(d, localAt + 28);
    const dataAt = localAt + 30 + localNameLen + localExtraLen;
    if (dataAt + compressedSize > cdStart ||
        UTF8.decode(b.subarray(localAt + 30, localAt + 30 + localNameLen)) !== name)
      reject("ZIP verisi taşmış veya dosya adı farklı.");
    const raw = b.slice(dataAt, dataAt + compressedSize);
    let result: Uint8Array;
    if (method === 0) result = raw;
    else {
      try {
        const stream = new Blob([raw.buffer as ArrayBuffer]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
        const reader = stream.getReader();
        const chunks: Uint8Array[] = [];
        let length = 0;
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          length += value.length;
          if (length > plainSize || length > SIZES[name]) {
            await reader.cancel();
            reject("Açılan ZIP beklenen boyutu aşıyor.");
          }
          chunks.push(value);
        }
        result = new Uint8Array(length);
        let offset = 0;
        for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; }
      } catch { reject("Sıkıştırılmış ZIP çözülemedi."); }
    }
    if (result.length !== plainSize || crc32(result) !== crc)
      reject("ZIP CRC veya dosya boyutu uyuşmuyor.");
    files[name] = result;
    at = next;
  }
  if (at !== end || Object.keys(files).length !== 3) reject("Paket eksik.");
  return files;
}

/** Never trust labels alone: validate format, target, byte sizes and SHA-256. */
export async function inspectFirmwarePackage(archive: Uint8Array): Promise<FirmwarePackage> {
  const files = await filesOfZip(archive);
  let meta: unknown;
  try { meta = JSON.parse(UTF8.decode(files["orun-package.json"])); }
  catch { reject("ORUN paket bilgisi okunamadı."); }
  if (!meta || typeof meta !== "object") reject("Paket bilgisi yok.");
  const m = meta as Record<string, unknown>;
  const target = m.target as Record<string, unknown> | undefined;
  const app = m.application as Record<string, unknown> | undefined;
  if (m.format !== "orun-serial-dfu" || m.version !== 1 ||
      !target || target.board !== "RAK4630" || target.chip !== "nRF52840" || target.env !== "rak4630_orun" ||
      typeof m.source_commit !== "string" || !/^[0-9a-f]{40}$/.test(m.source_commit) ||
      !app || app.bin !== "application.bin" || app.dat !== "application.dat")
    reject("Hedef, format veya kaynak sürümü uyuşmuyor.");
  const image = files["application.bin"], initPacket = files["application.dat"];
  const [imageSha256, initSha256] = await Promise.all([sha256(image), sha256(initPacket)]);
  if (app.bin_size !== image.length || app.dat_size !== initPacket.length ||
      app.bin_sha256 !== imageSha256 || app.dat_sha256 !== initSha256)
    reject("SHA-256 veya dosya boyutları uyuşmuyor.");
  return { sourceCommit: m.source_commit, target: "RAK4630", image, initPacket, imageSha256, initSha256 };
}
