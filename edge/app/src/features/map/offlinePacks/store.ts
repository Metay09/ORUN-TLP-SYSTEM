// Persistent ORUN map archives, *separate from the collar observation ledger*.
// Download is explicit and only from validated HTTPS package manifests.
// No automatic tile scraping; no remote URL is opened until user chooses a pack.
import { sha256 } from "@noble/hashes/sha2.js";
import {
  checkInstallSpace, packFileName, parseOfflinePack,
  type InstalledPack, type OfflinePack,
} from "./catalog.ts";

const ROOT_NAME = "orun-offline-maps-v1";
const META_SUFFIX = ".installed.json";
const hex = (buf: Uint8Array): string => Array.from(buf, (x) => x.toString(16).padStart(2, "0")).join("");

export interface InstallProgress { receivedBytes: number; totalBytes: number; }
export type ProgressHandler = (progress: InstallProgress) => void;

export function supportsOfflineMapStorage(): boolean {
  return typeof navigator !== "undefined" && typeof navigator.storage?.getDirectory === "function" &&
    typeof navigator.storage?.estimate === "function";
}

async function mapsDir(): Promise<FileSystemDirectoryHandle> {
  if (!supportsOfflineMapStorage())
    throw new Error("Bu Android WebView sürümünde kalıcı harita dosyası desteği yok.");
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(ROOT_NAME, { create: true });
}

async function readMetadata(
  dir: FileSystemDirectoryHandle, metaName: string,
): Promise<InstalledPack | null> {
  let handle: FileSystemFileHandle;
  try { handle = await dir.getFileHandle(metaName); }
  catch (error) {
    if ((error as DOMException).name === "NotFoundError") return null;
    throw error;
  }
  const json: unknown = JSON.parse(await (await handle.getFile()).text());
  if (!json || typeof json !== "object" || Array.isArray(json))
    throw new Error("Harita paketi kayıt bilgisi bozuk.");
  const row = json as Record<string, unknown>;
  const pack = parseOfflinePack(row.pack);
  if (!Number.isSafeInteger(row.installedAt) || (row.installedAt as number) < 0 ||
      typeof row.fileName !== "string" || row.fileName !== packFileName(pack))
    throw new Error("Harita paketi kayıt bilgisi uyumsuz.");
  const file = await (await dir.getFileHandle(row.fileName)).getFile();
  if (file.size !== pack.bytes) throw new Error("Harita dosyası boyutu uyuşmuyor.");
  if (!(await isPmtilesFile(file))) throw new Error("Geçersiz harita dosyası başlığı.");
  return { pack, installedAt: row.installedAt as number, fileName: row.fileName };
}

export async function listInstalledPacks(): Promise<InstalledPack[]> {
  const dir = await mapsDir();
  // Async entries() exists in Chromium OPFS; DOM typings historically omitted
  // it in some TS versions, so state the interface explicitly.
  const d = dir as FileSystemDirectoryHandle & {
    entries(): AsyncIterableIterator<[string, FileSystemHandle]>;
  };
  const installed: InstalledPack[] = [];
  for await (const [name, handle] of d.entries()) {
    if (handle.kind !== "file" || !name.endsWith(META_SUFFIX)) continue;
    try {
      const p = await readMetadata(dir, name);
      if (p) installed.push(p);
    } catch {
      // Fail closed: corrupt or interrupted packs never appear as installed.
      // Never erase these files automatically; preserve them for recovery.
    }
  }
  return installed.sort((a, b) => a.pack.title.localeCompare(b.pack.title, "tr"));
}

async function isPmtilesFile(file: File): Promise<boolean> {
  if (file.size < 127) return false;
  const buf = new Uint8Array(await file.slice(0, 8).arrayBuffer());
  return buf.length === 8 && String.fromCharCode(...buf.slice(0, 7)) === "PMTiles" && buf[7] === 3;
}

/** Streams, hashes and writes chunks one by one. It never buffers a GB pack
 * in JS memory. An error cannot mark the file as a verified installed pack.
 */
export async function streamVerifiedPmtiles(
  input: ReadableStream<Uint8Array>, writer: Pick<FileSystemWritableFileStream, "write">,
  expected: OfflinePack, signal?: AbortSignal, onProgress?: ProgressHandler,
): Promise<void> {
  const hash = sha256.create();
  const reader = input.getReader();
  let size = 0;
  let firstEight = new Uint8Array(0);
  try {
    for (;;) {
      if (signal?.aborted) throw new DOMException("İndirme iptal edildi.", "AbortError");
      const next = await reader.read();
      if (next.done) break;
      const bytes = next.value;
      size += bytes.byteLength;
      if (size > expected.bytes) throw new Error("İndirilen paket beklenen boyutu aştı.");
      if (firstEight.length < 8) {
        const peek = new Uint8Array(Math.min(8 - firstEight.length, bytes.byteLength));
        peek.set(bytes.subarray(0, peek.length));
        const combined = new Uint8Array(firstEight.length + peek.length);
        combined.set(firstEight);
        combined.set(peek, firstEight.length);
        firstEight = combined;
      }
      hash.update(bytes);
      await writer.write(new Uint8Array(bytes));
      onProgress?.({ receivedBytes: size, totalBytes: expected.bytes });
    }
  } finally {
    reader.releaseLock();
  }
  if (size !== expected.bytes) throw new Error("Harita paketi eksik indi.");
  if (firstEight.length !== 8 || String.fromCharCode(...firstEight.slice(0, 7)) !== "PMTiles" ||
      firstEight[7] !== 3) throw new Error("Dosya PMTiles v3 biçiminde değil.");
  if (hex(hash.digest()) !== expected.sha256)
    throw new Error("Harita paketi SHA-256 doğrulaması başarısız.");
}

/** Download preassembled/authorized single-file regional maps. Does not
 * inspect the user's private ledger or upload any location data.
 */
export async function installOfflinePack(
  raw: OfflinePack, onProgress?: ProgressHandler, signal?: AbortSignal,
): Promise<InstalledPack> {
  const pack = parseOfflinePack(raw);
  const dir = await mapsDir();
  const fileName = packFileName(pack);
  const metaName = fileName + META_SUFFIX;
  const existing = await readMetadata(dir, metaName);
  if (existing) return existing;

  const space = checkInstallSpace(pack, await navigator.storage.estimate());
  if (space) throw new Error(space);
  if (signal?.aborted) throw new DOMException("İndirme iptal edildi.", "AbortError");

  const response = await fetch(pack.url, { signal, cache: "no-store" });
  if (!response.ok || !response.body) throw new Error(`Harita sunucusu cevap vermedi: HTTP ${response.status}`);
  if (!response.url.startsWith("https://"))
    throw new Error("Harita sunucusu güvenli HTTPS yanıtı vermedi.");
  if (response.headers.get("content-encoding") &&
      response.headers.get("content-encoding") !== "identity")
    throw new Error("Harita paketi aktarımı sıkıştırılmamalı.");
  const declared = response.headers.get("content-length");
  if (declared && Number(declared) !== pack.bytes)
    throw new Error("Harita sunucusunun bildirdiği dosya boyutu farklı.");

  let committed = false;
  const handle = await dir.getFileHandle(fileName, { create: true });
  try {
    const writable = await handle.createWritable({ keepExistingData: false });
    try { await streamVerifiedPmtiles(response.body, writable, pack, signal, onProgress); }
    finally { await writable.close(); }

    const file = await handle.getFile();
    if (file.size !== pack.bytes || !(await isPmtilesFile(file)))
      throw new Error("İndirilen harita dosyası kaydedilemedi.");

    const install: InstalledPack = { pack, fileName, installedAt: Date.now() };
    // Installed status is committed *only* after file has been checksummed,
    // closed and re-read. A crash prior to this leaves an ignored orphan file.
    const meta = await dir.getFileHandle(metaName, { create: true });
    const out = await meta.createWritable({ keepExistingData: false });
    try { await out.write(JSON.stringify(install)); }
    finally { await out.close(); }
    committed = true;
    return install;
  } finally {
    if (!committed) {
      try { await dir.removeEntry(fileName); } catch { /* preserve original error */ }
    }
  }
}

export async function installedPackFile(pack: InstalledPack): Promise<File> {
  const dir = await mapsDir();
  const expected = await readMetadata(dir, pack.fileName + META_SUFFIX);
  if (!expected || expected.pack.sha256 !== pack.pack.sha256)
    throw new Error("Bu harita paketi telefonda doğrulanmadı.");
  return (await dir.getFileHandle(expected.fileName)).getFile();
}

export async function removeOfflinePack(pack: InstalledPack): Promise<void> {
  const dir = await mapsDir();
  const metaName = pack.fileName + META_SUFFIX;
  const expected = await readMetadata(dir, metaName);
  if (!expected || expected.pack.sha256 !== pack.pack.sha256)
    throw new Error("Silinecek harita paketi doğrulanamadı.");
  // Metadata first: even if the file cannot be deleted, it is no longer
  // eligible to be used as a complete offline pack.
  await dir.removeEntry(metaName);
  await dir.removeEntry(pack.fileName);
}
