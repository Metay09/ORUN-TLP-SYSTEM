// 4c: kullanıcı telefondaki ORUN paketini seçip doğrulayabilir.
// Bu ekran sadece dosya bütünlüğünü inceler; USB/DFU yüklemesi yapmaz.
import { useState } from "react";
import { inspectFirmwarePackage, type FirmwarePackage } from "../../app/firmwarePackage.ts";
import { Dialog, KV } from "../../ds/components.tsx";

export function FirmwarePackageDialog({ onClose }: { onClose: () => void }) {
  const [pkg, setPkg] = useState<FirmwarePackage | null>(null);
  const [fileName, setFileName] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  async function inspect(file: File | undefined): Promise<void> {
    setPkg(null);
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
