// Açılış ekranı (sahip kararı 2026-11-14): Android'in kendi açılış ekranındaki
// ORUN işaretinin (res/drawable/splash_mark.xml) tam üstünde başlar, aynı yer
// ve boyutta; etrafında halkalar dönerek tekrar eder. Kalkar:
//   - güncel konumlar ekrana gelince (opening.ts: m.starting ve m.settling biter),
//   - internet yoksa eşitleme hata verir vermez,
//   - her durumda en geç 10 saniyede (kilitlenmez).
// Yalnız soğuk açılışta: arka plandan dönüşte gösterilmez ("yenileniyor" yazar).
import { useEffect, useState } from "react";
import { REFRESH_LIMIT_MS } from "../../app/opening.ts";

// The same pin as the status-bar icon (ic_stat_orun.xml), 24 -> 288 box.
const PIN = "M12,2C8.13,2 5,5.13 5,9c0,5.25 7,13 7,13s7,-7.75 7,-13c0,-3.87 -3.13,-7 -7,-7zM12,11.5c-1.38,0 -2.5,-1.12 -2.5,-2.5s1.12,-2.5 2.5,-2.5 2.5,1.12 2.5,2.5 -1.12,2.5 -2.5,2.5z";

export function BootSplash({ ready }: { ready: boolean }) {
  const [phase, setPhase] = useState<"on" | "leaving" | "gone">("on");
  const [slow, setSlow] = useState(false);
  useEffect(() => {
    const s = setTimeout(() => setSlow(true), 1500);
    const t = setTimeout(() => setPhase((p) => (p === "on" ? "leaving" : p)), REFRESH_LIMIT_MS);
    return () => { clearTimeout(s); clearTimeout(t); };
  }, []);
  useEffect(() => { if (ready) setPhase((p) => (p === "on" ? "leaving" : p)); }, [ready]);
  useEffect(() => {
    if (phase !== "leaving") return;
    const t = setTimeout(() => setPhase("gone"), 320);
    return () => clearTimeout(t);
  }, [phase]);
  if (phase === "gone") return null;
  return (
    <div className={"boot" + (phase === "leaving" ? " out" : "")} role="status" aria-label="ORUN açılıyor">
      <svg className="boot-mark" viewBox="0 0 288 288" width="288" height="288" aria-hidden="true">
        <circle className="boot-ping" cx="144" cy="126" r="40" />
        <circle className="boot-ping b" cx="144" cy="126" r="40" />
        <g transform="translate(72 72) scale(6)"><path d={PIN} /></g>
      </svg>
      <p className={"boot-note" + (slow ? " on" : "")}>Güncel konumlar alınıyor…</p>
    </div>
  );
}
