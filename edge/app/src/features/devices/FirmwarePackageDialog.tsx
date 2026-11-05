// 4c: kullanıcı telefondaki ORUN paketini seçip doğrulayabilir.
// Bu ekran sadece dosya bütünlüğünü inceler; USB/DFU yüklemesi yapmaz.
import { useState } from "react";
import { UsbSerial } from "@leeskies/capacitor-usb-serial";
import { readFirmwareEvidence, type FirmwareEvidence } from "../../app/firmwareSafety.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { inspectFirmwarePackage, type FirmwarePackage } from "../../app/firmwarePackage.ts";
import { Dialog, KV } from "../../ds/components.tsx";

export function FirmwarePackageDialog({ onClose, device }: { onClose: () => void; device: string }) {
  const [pkg, setPkg] = useState<FirmwarePackage | null>(null);
  const [fileName, setFileName] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [checking, setChecking] = useState(false);
  const [baseline, setBaseline] = useState<FirmwareEvidence | null>(null);

  async function inspect(file: File | undefined): Promise<void> {
    setPkg(null);
    setBaseline(null);
    setError("");
    setFileName(file?.name ?? "");
    if (!file) return;
    setBusy(true);
    try {
      if (file.size > 2 * 1024 * 1024) throw new Error("Dosya beklenenden büyük.");
      const parsed = await inspectFirmwarePackage(new Uint8Array(await file.arrayBuffer()));
      setPkg(parsed);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Yazılım paketi okunamadı.");
    } finally {
      setBusy(false);
    }
  }

  // Read-only baseline: no bootloader reset or flash operation.
  async function checkUsb(): Promise<void> {
    setChecking(true); setError(""); setBaseline(null);
    let con: UsbConsole | null = null;
    try {
      const { devices } = await UsbSerial.listDevices();
      const rak = devices.filter((d) => d.vendorId === 0x239a && d.productId === 0x8029);
      const bySerial = rak.filter((d) => d.serialNumber?.toUpperCase() === device.toUpperCase());
      const target = bySerial.length === 1 ? bySerial[0] : rak.length === 1 ? rak[0] : null;
      if (!target) throw new Error(rak.length === 0
        ? "USB'de RAK4630 uygulaması yok. OTG kablosunu kontrol et."
        : "Birden fazla RAK var, doğru cihaz güvenle seçilemedi. Yalnız hedef cihazı bağla.");
      con = await UsbConsole.open(target.deviceId);
      const evidence = await readFirmwareEvidence(con);
      if (evidence.id !== device.toUpperCase())
        throw new Error("USB'deki cihazın kimliği seçilen tasma/gateway ile uyuşmuyor. Hiçbir işlem yapılmadı.");
      setBaseline(evidence);
    } catch (e) {
      setError(e instanceof Error ? e.message : "USB cihazı okunamadı.");
    } finally {
      if (con) await con.close();
      setChecking(false);
    }
  }

  return (
    <Dialog title="Yazılım paketini denetle" onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 20px" }}>
        <p className="sm" style={{ margin: 0 }}>
          Bilgisayarda hazırlanmış ORUN .zip dosyasını seç.
          Dosyanın RAK4630 için olduğunu ve içindeki uygulama yazılımının SHA-256 değerlerini kontrol eder.
        </p>
        <label className="sm" htmlFor="orun-firmware-file">ORUN yazılım paketi (.zip)</label>
        <input id="orun-firmware-file" className="inp" type="file"
          accept=".zip,application/zip" disabled={busy}
          onChange={(e) => { void inspect(e.currentTarget.files?.[0]); }} />
        {busy && <p role="status" className="sm">Paket doğrulanıyor…</p>}
        {error && <p role="alert" className="sm t-aging">{error}</p>}
        {pkg && (
          <div className="card">
            <KV k="Dosya">{fileName}</KV>
            <KV k="Hedef">{pkg.target} / nRF52840</KV>
            <KV k="Derleme">{pkg.sourceCommit.slice(0, 12)}</KV>
            <KV k="Yazılım boyutu">{pkg.image.length.toLocaleString("tr-TR")} bayt</KV>
            <KV k="Başlatma bilgisi">{pkg.initPacket.length.toLocaleString("tr-TR")} bayt</KV>
            <p role="status" className="sm" style={{ padding: "8px 16px", margin: 0 }}>
              Paket dosyalarının bütünlüğü doğrulandı. Cihaza yazılım yüklenmedi.
            </p>
          </div>
        )}
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" type="button" disabled={!pkg || busy || checking} onClick={() => { void checkUsb(); }}>
            {checking ? "USB cihazı okunuyor…" : "USB'deki cihazı kontrol et"}
          </button>
        </div>
        {baseline && (
          <div className="card">
            <KV k="Cihaz kimliği">{baseline.id}</KV>
            <KV k="Cihaz türü">{baseline.kind === "tracker" ? "Tasma" : "Gateway"}</KV>
            <KV k="Cihazdaki yazılım">{baseline.firmware ?? "—"}</KV>
            <KV k="Kayıt durumu">{baseline.kind === "tracker"
              ? "Kayıt sırası " + (baseline.nextSeq ?? "?") + " · hafıza hazır"
              : "Gateway hafızası: " + (baseline.custodyRegion ?? "?")}</KV>
            <p role="status" className="sm" style={{ padding: "8px 16px", margin: 0 }}>
              USB cihazının kimliği ve mevcut durumu okundu. Henüz yazılım yüklenmedi.
            </p>
          </div>
        )}
        <p className="fine" style={{ margin: 0 }}>
          Bu işlem cihaz kimliğini, anahtarları veya kayıtları değiştirmez.
          Gerçek USB yüklemesi ve yükleme sonrası kontrol henüz etkin değil.
        </p>
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" type="button" onClick={onClose}>Kapat</button>
        </div>
      </div>
    </Dialog>
  );
}
