// 2d: cihaza ad ver (hesapta saklanır). Boş bırakılırsa Bluetooth adı görünür.
import { useState } from "react";
import { bleName } from "../../app/deviceNames.ts";
import { useBack } from "../../app/nav.ts";
import { pilotSetDeviceName } from "../../app/ownerAccountPilotRuntime.ts";
import { Dialog } from "../../ds/components.tsx";

export function NameDialog({ device, kind, current, onClose }: {
  device: string; kind: "gateway" | "tracker"; current: string | null; onClose: () => void;
}) {
  const [name, setName] = useState(current ?? "");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useBack(onClose);
  async function save(): Promise<void> {
    setBusy(true); setError("");
    try { await pilotSetDeviceName(device, name); onClose(); }
    catch (e) { setError(e instanceof Error ? e.message : "Ad kaydedilemedi."); }
    finally { setBusy(false); }
  }
  return (
    <Dialog title={kind === "gateway" ? "Gateway'e ad ver" : "Tasmaya ad ver"} onClose={onClose}>
      <form className="stack" style={{ padding: "0 20px 16px" }} onSubmit={(e) => { e.preventDefault(); if (!busy) void save(); }}>
        <label className="lab">{kind === "gateway" ? "Gateway'in adı (ör. Tepe, Ev)" : "Hayvanın adı"}
          <input className="inp" value={name} maxLength={40} autoFocus disabled={busy}
            placeholder={bleName(device)} onChange={(e) => setName(e.target.value)} />
        </label>
        <p className="fine" style={{ margin: 0 }}>Ad hesabında saklanır, bütün telefonlarda görünür. Boş bırakırsan {bleName(device)} görünür.</p>
        {error && <p role="alert" className="sm t-aging">{error}</p>}
        <div className="btns">
          <button className="btn" type="button" onClick={onClose}>Şimdi değil</button>
          <button className="btn primary" type="submit" disabled={busy}>{busy ? "Kaydediliyor…" : "Kaydet"}</button>
        </div>
      </form>
    </Dialog>
  );
}
