// 4c: DFU update orchestration. No automatic retries or silent success.
// Every mutating attempt is recorded BEFORE entering the bootloader; pending
// attempts survive app restarts and can only be reconciled by read-only checks.
import { compareFirmwareEvidence, type FirmwareEvidence } from "./firmwareSafety.ts";
import { transferDfu, type DfuProgress, type DfuSerial } from "./firmwareUsb.ts";
import type { FirmwarePackage } from "./firmwarePackage.ts";

export type UpdatePhase = "uncertain" | "transferring" | "verifying" | "confirmed";
export interface UpdateRecord {
  device: string;
  sourceCommit: string;
  imageSha256: string;
  phase: UpdatePhase;
  bytesAcknowledged: number;
  imageBytes: number;
  updatedAt: number;
  note: string;
  before: FirmwareEvidence;
}
export interface JournalStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}
export interface FirmwareUpdatePorts {
  readApplication(id: string): Promise<FirmwareEvidence>;
  enterBootloader(id: string): Promise<DfuSerial & { close(): Promise<void> }>;
}
const KEY = "orun.firmwareUpdate.v1";
const ID = /^[0-9A-F]{16}$/;

function key(id: string): string { return KEY + "." + id; }
export function previousFirmwareUpdate(store: JournalStorage, id: string): UpdateRecord | null {
  const name = id.toUpperCase();
  if (!ID.test(name)) return null;
  try {
    const raw = store.getItem(key(name));
    if (!raw) return null;
    const record = JSON.parse(raw) as UpdateRecord;
    return record.device === name && /^[0-9a-f]{40}$/.test(record.sourceCommit) &&
      /^(uncertain|transferring|verifying|confirmed)$/.test(record.phase) &&
      record.before?.id === name ? record : null;
  } catch { return null; }
}
function save(store: JournalStorage, record: UpdateRecord, phase: UpdatePhase, note: string): void {
  record.phase = phase;
  record.note = note;
  record.updatedAt = Date.now();
  // Fail closed if state cannot be persisted. Do not touch flash after failure.
  store.setItem(key(record.device), JSON.stringify(record));
}
export function verifyAfter(record: UpdateRecord, after: FirmwareEvidence): string[] {
  return compareFirmwareEvidence(record.before, after, record.sourceCommit).reasons;
}
export type UpdateEvent = { phase: UpdatePhase; progress: DfuProgress | null; message: string };

/** Called only by an explicit operator action (never at app startup). */
export async function updateFirmware(
  store: JournalStorage, ports: FirmwareUpdatePorts,
  id: string, pkg: FirmwarePackage, onEvent: (e: UpdateEvent) => void = () => {},
): Promise<UpdateRecord> {
  const device = id.toUpperCase();
  if (!ID.test(device) || pkg.target !== "RAK4630" ||
      !/^[0-9a-f]{40}$/.test(pkg.sourceCommit) ||
      !/^[0-9a-f]{64}$/.test(pkg.imageSha256))
    throw new Error("Hedef cihaz veya firmware paketi doğrulanamadı.");
  const previous = previousFirmwareUpdate(store, device);
  if (previous && previous.phase !== "confirmed")
    throw new Error("Önceki firmware güncellemesi sonuç-belirsiz. Önce cihazı yeniden denetle; otomatik tekrar yok.");

  // Fresh readback immediately before touch: UI's old preview is not evidence.
  const before = await ports.readApplication(device);
  if (before.id !== device) throw new Error("USB cihaz kimliği hedefle uyuşmuyor.");
  const record: UpdateRecord = {
    device, sourceCommit: pkg.sourceCommit, imageSha256: pkg.imageSha256,
    phase: "uncertain", bytesAcknowledged: 0, imageBytes: pkg.image.length,
    updatedAt: Date.now(), note: "Bootloader'a geçmeden önce kaydedildi.", before,
  };
  save(store, record, "uncertain", "Giriş başladı; sonraki sonuç doğrulanmadan başarı sayılmaz.");
  onEvent({ phase: record.phase, progress: null, message: "Bootloader'a geçiliyor…" });

  let link: (DfuSerial & { close(): Promise<void> }) | null = null;
  try {
    link = await ports.enterBootloader(device);
    save(store, record, "transferring", "Firmware paketleri aktarılıyor.");
    await transferDfu(link, pkg, (progress) => {
      if (progress.done > record.bytesAcknowledged) {
        record.bytesAcknowledged = progress.done;
        save(store, record, "transferring", "Paket alındı; cihaz doğrulaması bekleniyor.");
      }
      onEvent({ phase: "transferring", progress, message: "Firmware gönderiliyor…" });
    });
  } catch (err) {
    save(store, record, "uncertain", "DFU sonucu bilinmiyor; yeniden yükleme yapma.");
    throw err;
  } finally {
    if (link) await link.close().catch(() => undefined);
  }
  save(store, record, "verifying", "Aktarım bitti; cihaz yeniden okunuyor.");
  onEvent({ phase: record.phase, progress: null, message: "Cihaz yeniden okunuyor…" });
  return reconcileFirmwareUpdate(store, ports, device);
}

/** Read-only recovery after a crash, disconnect or app restart. Never reflashes. */
export async function reconcileFirmwareUpdate(
  store: JournalStorage, ports: Pick<FirmwareUpdatePorts, "readApplication">, id: string,
): Promise<UpdateRecord> {
  const record = previousFirmwareUpdate(store, id);
  if (!record) throw new Error("Bu cihaz için korunmuş güncelleme kaydı bulunamadı.");
  // A version match alone is insufficient: all key/grant/store invariants must hold.
  let after: FirmwareEvidence;
  try { after = await ports.readApplication(record.device); }
  catch (err) {
    save(store, record, "uncertain", "Cihaz yeniden okunamadı; sonuç henüz doğrulanmadı.");
    throw err;
  }
  const problems = verifyAfter(record, after);
  if (problems.length) {
    save(store, record, "uncertain", "Koruma denetimi başarısız: " + problems.join(" "));
    throw new Error(record.note);
  }
  save(store, record, "confirmed", "Yazılım sürümü, cihaz kimliği, anahtar ve kayıt denetimi geçti.");
  return record;
}
