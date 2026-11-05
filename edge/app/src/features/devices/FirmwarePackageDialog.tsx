// 4c: USB firmware update surface. Flashing is EXPERIMENTAL and disabled
// in production builds; package inspection and read-only reconciliation work
// regardless of the build flag. No auto-retry after an interrupted update.
import { useState } from "react";
import { inspectFirmwarePackage, type FirmwarePackage } from "../../app/firmwarePackage.ts";
import { previousFirmwareUpdate, reconcileFirmwareUpdate, updateFirmware, type UpdateEvent, type UpdateRecord } from "../../app/firmwareUpdate.ts";
import { type FirmwareEvidence } from "../../app/firmwareSafety.ts";
import { readOrunApplication, readAfterOrunUpdate, enterOrunBootloader } from "../../platform/firmwareUsbBridge.ts";
import { Dialog, KV } from "../../ds/components.tsx";

const ENABLE_EXPERIMENTAL_USB_FLASH = import.meta.env.VITE_ORUN_USB_FLASH_EXPERIMENTAL === "1";
const STAGE: Record<string, string> = {
  init: "Başlatılıyor", erase: "Firmware alanı hazırlanıyor",
  transfer: "Yazılım aktarılıyor", activate: "Cihaz başlatılıyor",
};
const textError = (err: unknown) => err instanceof Error ? err.message : "Güncelleme doğrulanamadı.";

export function FirmwarePackageDialog({ onClose, device }: { onClose: () => void; device: string }) {
  const [pkg, setPkg] = useState<FirmwarePackage | null>(null);
  const [fileName, setFileName] = useState("");
  const [error, setError] = useState("");
  const [message, setMessage] = useState("");
  const [working, setWorking] = useState(false);
  const [baseline, setBaseline] = useState<FirmwareEvidence | null>(null);
  const [approved, setApproved] = useState(false);
  const [event, setEvent] = useState<UpdateEvent | null>(null);
  const [record, setRecord] = useState<UpdateRecord | null>(() => previousFirmwareUpdate(localStorage, device));
  const close = () => { if (!working) onClose(); };

  async function inspect(file: File | undefined): Promise<void> {
    setPkg(null); setBaseline(null); setApproved(false); setError(""); setMessage("");
    setFileName(file?.name ?? "");
    if (!file) return;
    setWorking(true);
    try {
      if (file.size > 2 * 1024 * 1024) throw new Error("Dosya beklenenden büyük.");
      setPkg(await inspectFirmwarePackage(new Uint8Array(await file.arrayBuffer())));
    } catch (err) { setError(textError(err)); }
    finally { setWorking(false); }
  }

  async function checkUsb(): Promise<void> {
    setWorking(true); setError(""); setMessage(""); setBaseline(null);
    try {
      const evidence = await readOrunApplication(device);
      setBaseline(evidence);
      setMessage("USB'deki cihazın kimliği ve hafızası okundu; cihaza yazılmadı.");
    } catch (err) { setError(textError(err)); }
    finally { setWorking(false); }
  }

  async function start(): Promise<void> {
    if (!ENABLE_EXPERIMENTAL_USB_FLASH || !approved || !baseline || !pkg || working) return;
    setWorking(true); setError(""); setMessage(""); setEvent(null);
    try {
      const result = await updateFirmware(localStorage, {
        readApplication: (id) => readOrunApplication(id),
        readAfterUpdate: (id) => readAfterOrunUpdate(id),
        enterBootloader: (id) => enterOrunBootloader(id),
      }, device, pkg, setEvent);
      setRecord(result);
      setMessage("Doğrulandı: yazılım sürümü, cihaz kimliği, anahtar dönemi ve kayıtlar korundu.");
    } catch (err) {
      setError(textError(err));
      setRecord(previousFirmwareUpdate(localStorage, device));
    } finally { setWorking(false); setApproved(false); }
  }

  async function reconcile(): Promise<void> {
    if (working) return;
    setWorking(true); setError(""); setMessage("");
    try {
      const result = await reconcileFirmwareUpdate(localStorage, {
        readApplication: (id) => readAfterOrunUpdate(id),
      }, device);
      setRecord(result);
      setMessage("Güncelleme yeniden okunarak doğrulandı. Yeni firmware gönderilmedi.");
    } catch (err) {
      setError(textError(err));
      setRecord(previousFirmwareUpdate(localStorage, device));
    } finally { setWorking(false); }
  }

  const unresolved = !!record && record.phase !== "confirmed";
  const progress = event?.progress;
  const percent = progress?.total ? Math.floor(100 * progress.done / progress.total) : 0;

  return (
    <Dialog title="USB yazılım güncelleme" onClose={close}>
      <div className="stack" style={{ padding: "0 20px 20px" }}>
        <p className="sm" style={{ margin: 0 }}>
          Yalnız cihazı kontrol etmek için ZIP seçmen gerekmez. USB kimliğini,
          mevcut yazılımı ve kayıt durumunu okuyabilirsin; cihaza hiçbir şey yazılmaz.
          Firmware güncellemesi için ORUN paketini (.zip) ayrıca seçebilirsin.
          ZIP bütünlüğü yazılımın imzalı olduğunu tek başına kanıtlamaz.
        </p>
        <label className="sm" htmlFor="orun-firmware-file">ORUN yazılım paketi</label>
        <input id="orun-firmware-file" className="inp" type="file" accept=".zip,application/zip"
          disabled={working} onChange={(e) => { void inspect(e.currentTarget.files?.[0]); }} />
        {pkg && (
          <div className="card">
            <KV k="Dosya">{fileName}</KV>
            <KV k="Hedef">RAK4630 / nRF52840</KV>
            <KV k="Kaynak sürüm">{pkg.sourceCommit.slice(0, 12)}</KV>
            <KV k="Uygulama boyutu">{pkg.image.length.toLocaleString("tr-TR")} bayt</KV>
            <KV k="Paket durumu">CRC32 ve SHA-256 doğrulandı</KV>
          </div>
        )}
        <div className="btns" style={{ padding: 0 }}>
          <button type="button" className="btn" disabled={working}
            onClick={() => { void checkUsb(); }}>USB'deki cihazı kontrol et</button>
        </div>
        {baseline && (
          <div className="card">
            <KV k="USB cihaz kimliği">{baseline.id}</KV>
            <KV k="Tür">{baseline.kind === "tracker" ? "Tasma" : "Gateway"}</KV>
            <KV k="Mevcut yazılım">{baseline.firmware ?? "—"}</KV>
            <KV k="Kayıt hafızası">{baseline.kind === "tracker"
              ? "Sıra " + (baseline.nextSeq ?? "?") + " · hazır"
              : baseline.custodyRegion ?? "Bilinmiyor"}</KV>
          </div>
        )}
        {record && (
          <div className="card">
            <KV k="Son deneme">{record.sourceCommit.slice(0, 12)}</KV>
            <KV k="Aktarılan">{record.bytesAcknowledged.toLocaleString("tr-TR")} / {record.imageBytes.toLocaleString("tr-TR")} bayt</KV>
            <KV k="Sonuç">{record.phase === "confirmed" ? "Cihazdan doğrulandı" : "Doğrulanmadı"}</KV>
            <p className="sm" style={{ padding: "8px 16px", margin: 0 }}>{record.note}</p>
          </div>
        )}
        {unresolved && (
          <div className="btns" style={{ padding: 0 }}>
            <button type="button" className="btn" disabled={working} onClick={() => { void reconcile(); }}>
              Cihazı yeniden denetle (yazılım göndermez)
            </button>
          </div>
        )}
        {ENABLE_EXPERIMENTAL_USB_FLASH ? (
          <>
            <p className="fine" style={{ margin: 0 }}>
              Deneysel USB aktarımı: yalnız geliştirme cihazında deneyin. Kabloyu veya gücü kesmeyin.
              Başlamadan önce hedef ve paket yeniden okunur; sonuç okunamazsa tekrar otomatik yükleme yapılmaz.
            </p>
            <label className="sm">
              <input type="checkbox" checked={approved} disabled={working || unresolved}
                onChange={(e) => setApproved(e.currentTarget.checked)} />
              Bu cihazda deneysel firmware yüklemesini açıkça onaylıyorum.
            </label>
            <div className="btns" style={{ padding: 0 }}>
              <button className="btn primary" type="button"
                disabled={working || !approved || !pkg || !baseline || unresolved}
                onClick={() => { void start(); }}>Firmware'i USB ile yükle</button>
            </div>
          </>
        ) : (
          <p className="fine" style={{ margin: 0 }}>
            Bu yapıda USB aktarımı etkin değil. Paket ve USB ön denetimi yapılabilir;
            gerçek yükleme yalnız doğrulanmış özel test derlemesinde açılır.
          </p>
        )}
        {event && <p role="status" className="sm">
          {event.progress ? (STAGE[event.progress.stage] ?? event.message) + " · %" + percent : event.message}
        </p>}
        {message && <p role="status" className="sm">{message}</p>}
        {error && <p role="alert" className="sm t-aging">{error}</p>}
        {working && <p className="sm" role="status">İşlem sürüyor; uygulamayı kapatmayın.</p>}
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" type="button" disabled={working} onClick={close}>Kapat</button>
        </div>
      </div>
    </Dialog>
  );
}
