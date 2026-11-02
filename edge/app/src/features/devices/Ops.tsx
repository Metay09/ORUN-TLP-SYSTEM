// İşlemler: adım çubuğu, cihaz başına son işlemler, işlem ayrıntısı.
// "İşlendi" yalnız cihazın kendi onayıyla gelir (deviceOps.ts).
import { useSyncExternalStore } from "react";
import { bleName } from "../../app/deviceNames.ts";
import { groupLine, groupOps, KIND_WORD, STATE_WORD, steps, type OpTarget } from "../../app/deviceOps.ts";
import { fmt } from "../../app/i18n/tr.ts";
import { useBack } from "../../app/nav.ts";
import { deviceOps } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Cell, Dialog } from "../../ds/components.tsx";

export function useOps(): OpTarget[] {
  return useSyncExternalStore((fn) => deviceOps.subscribe(fn), () => deviceOps.snapshot());
}

const VIA: Record<string, string> = { usb: "USB ile", ble: "Bluetooth ile", lora: "LoRa ile" };

export function opSub(t: OpTarget): string {
  return [STATE_WORD[t.state], t.via ? VIA[t.via] : "", fmt.dateTime(t.updatedAt), t.note ?? ""].filter(Boolean).join(" · ");
}

/** Sırada ─ (Gateway'de ─) İşlendi */
export function OpSteps({ t }: { t: OpTarget }) {
  if (t.state === "superseded") return null;
  const s = steps(t);
  return (
    <div className="row" style={{ gap: 4, alignItems: "center", padding: "2px 0" }} aria-label={STATE_WORD[t.state]}>
      {s.map((x, i) => (
        <span key={x.label} className="row" style={{ gap: 4, alignItems: "center" }}>
          {i > 0 && <span style={{ width: 18, height: 2, background: x.on ? "var(--brand)" : "var(--line, #ccc)" }} />}
          <span className="fine" style={{ fontWeight: x.on ? 700 : 400, color: x.bad ? "var(--alarm-t)" : x.on ? "var(--brand-t)" : "var(--mute)" }}>{x.label}</span>
        </span>
      ))}
    </div>
  );
}

export function useLabel(): (device: string) => string {
  const m = useApp();
  return (device) => m.herd.find((a) => a.device?.toUpperCase() === device)?.name ?? bleName(device);
}

/** One operation, every target. */
export function OpDialog({ op, onClose }: { op: string; onClose: () => void }) {
  const rows = useOps().filter((r) => r.op === op);
  const label = useLabel();
  useBack(onClose);
  const g = groupOps(rows)[0];
  if (!g) return null;
  return (
    <Dialog title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`} onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <span className="sm">{groupLine(g)}</span>
        <div className="card" style={{ margin: 0 }}>
          {g.targets.map((t) => (
            <Cell key={t.device} title={label(t.device)} wrapSub sub={<>{opSub(t)}<OpSteps t={t} /></>}
              subTone={t.state === "rejected" ? "alarm" : t.state === "applied" || t.state === "same" ? "fresh" : undefined} />
          ))}
        </div>
        <p className="fine" style={{ margin: 0 }}>"Tasmaya işlendi" yalnız tasma onaylayınca yazılır. Sıradakiler tasmaya bağlanınca (şimdilik USB) kendiliğinden işlenir.</p>
      </div>
    </Dialog>
  );
}
