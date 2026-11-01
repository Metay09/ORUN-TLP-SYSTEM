// Sürü → "+": telefona USB ile bağlı tasmayı hesaba ekler (1d/1e).
// Tasma çiftliğin bütün gateway'lerine tek seferde yetkilendirilir.
import { useState } from "react";
import { Plus } from "@phosphor-icons/react";
import { addTrackerOverUsb, type AddStep } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { deviceNames, pilotAccountStatus, pilotDeviceSetupClient, pilotSetDeviceName } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Dialog } from "../../ds/components.tsx";
import { UsbConsole } from "../../platform/usbConsole.ts";

const STEP_TEXT: Record<AddStep, string> = {
  okunuyor: "Tasma okunuyor…",
  hazırlanıyor: "Sunucu kurulumu hazırlıyor…",
  yazılıyor: "Tasmaya yazılıyor…",
  onaylanıyor: "Onaylanıyor…",
};

export function AddTrackerDialog({ onClose }: { onClose: () => void }) {
  const m = useApp();
  const nav = useNav();
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState("");
  const [error, setError] = useState("");
  const [added, setAdded] = useState<string | null>(null);   // 2d: then ask its name
  const [name, setName] = useState("");
  const signedIn = Boolean(pilotAccountStatus());
  const gatewayOnUsb = m.station.link === "usb";
  useBack(() => { if (!busy) onClose(); });

  async function add(): Promise<void> {
    setBusy(true);
    setError("");
    setStatus("USB açılıyor…");
    let con: UsbConsole | null = null;
    try {
      con = await UsbConsole.open();
      const done = await addTrackerOverUsb(con, pilotDeviceSetupClient(),
        (s, detail) => setStatus(STEP_TEXT[s] + (detail && s === "yazılıyor" ? ` (${detail})` : "")));
      const ble = bleName(done.device);
      setStatus(done.added > 0
        ? `${ble} hesabına eklendi (${done.gateways.length} gateway). Gateway'in izni birkaç dakika içinde kendiliğinden güncellenir.`
        : `${ble} zaten hesabında; değişiklik gerekmedi.`);
      setAdded(done.device);
      setName(deviceNames.name(done.device) ?? "");
    } catch (e) {
      setStatus("");
      setError(e instanceof Error && e.message.length < 220 ? e.message : "Tasma eklenemedi.");
    } finally {
      if (con) await con.close();
      setBusy(false);
    }
  }

  return (
    <Dialog title={tr.add.title} onClose={() => { if (!busy) onClose(); }}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <p className="sm" style={{ margin: 0 }}>
          Tasmayı telefona USB (OTG) kablosuyla bağla ve ekle. Tasma çiftliğindeki bütün
          gateway'lerle kendiliğinden çalışır; gateway'lere ayrıca bir şey yapman gerekmez.
        </p>
        {!added && <button className="btn primary wide" type="button"
          disabled={busy || !signedIn || gatewayOnUsb} onClick={() => { void add(); }}>
          <Plus size={20} />{busy ? "Ekleniyor…" : "USB ile bağlı tasmayı ekle"}
        </button>}
        {!signedIn && (
          <button className="btn quiet wide" type="button" onClick={() => { onClose(); nav.push({ to: "settings" }); }}>
            Önce hesabınla giriş yap
          </button>
        )}
        {signedIn && gatewayOnUsb && <p className="fine">Önce gateway'in USB bağlantısını kes (Bağlantı sekmesi).</p>}
        {status && <p className="sm" role="status">{status}</p>}
        {added && (
          <form className="stack" onSubmit={(e) => {
            e.preventDefault();
            void pilotSetDeviceName(added, name).then(onClose, (err: unknown) =>
              setError(err instanceof Error ? err.message : "Ad kaydedilemedi."));
          }}>
            <label className="lab">Hayvanın adı (tasma da bu adla görünür)
              <input className="inp" value={name} maxLength={40} autoFocus placeholder={bleName(added)}
                onChange={(e) => setName(e.target.value)} />
            </label>
            <div className="btns">
              <button className="btn" type="button" onClick={onClose}>Şimdi değil</button>
              <button className="btn primary" type="submit">Kaydet</button>
            </div>
          </form>
        )}
        {error && <p role="alert" className="sm t-aging">{error}</p>}
        <p className="fine">{tr.add.after}</p>
      </div>
    </Dialog>
  );
}
