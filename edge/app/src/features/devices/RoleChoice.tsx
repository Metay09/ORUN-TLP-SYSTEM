// Y2a-2 (sahip kararı 2026-11-03): tek ORUN yazılımı. Boş (kurulmamış) cihaz
// takılınca ne olacağı sorulur; GPS takılıysa tasma, değilse gateway önerilir.
// Seçim cihaza yazılır, cihaz o rolde yeniden başlar; kurulmuş cihazın rolü
// sonradan ayarla değişmez.
import { CellTower, Cow } from "@phosphor-icons/react";
import { suggestedRole, type DeviceRole, type GnssHint } from "../../app/addTracker.ts";

const GPS_WORD: Record<GnssHint, string> = {
  yes: "Bu cihazda GPS var: büyük olasılıkla tasma.",
  no: "Bu cihazda GPS yok: büyük olasılıkla gateway (tasma konum bulamaz).",
  unknown: "GPS'e henüz bakılamadı.",
};

export function RoleChoice({ gnss, busy, onPick, onCancel }: {
  gnss: GnssHint; busy: boolean; onPick: (role: DeviceRole) => void; onCancel?: () => void;
}) {
  const hint = suggestedRole(gnss);
  const option = (role: DeviceRole) => (
    <button key={role} type="button" disabled={busy}
      className={"btn wide" + (hint === role || (!hint && role === "tasma") ? " primary" : "")}
      onClick={() => onPick(role)}>
      {role === "tasma" ? <Cow size={20} /> : <CellTower size={20} />}
      {role === "tasma" ? "Tasma" : "Gateway"}{hint === role ? " (önerilen)" : ""}
    </button>
  );
  return (
    <div className="stack" role="group" aria-label="Cihazın rolü">
      <p className="sm strong" style={{ margin: 0 }}>Bu cihaz henüz kurulmamış. Ne olarak kullanılacak?</p>
      <p className="sm" style={{ margin: 0 }}>{GPS_WORD[gnss]}</p>
      {hint === "gateway" ? [option("gateway"), option("tasma")] : [option("tasma"), option("gateway")]}
      <p className="fine" style={{ margin: 0 }}>
        Seçim cihaza yazılır ve cihaz yeniden başlar (USB izni yeniden sorulursa onayla). Sonradan
        değiştirmek için cihazın sıfırlanması gerekir.
      </p>
      {onCancel && <button type="button" className="btn quiet wide" disabled={busy} onClick={onCancel}>Şimdi değil</button>}
    </div>
  );
}
