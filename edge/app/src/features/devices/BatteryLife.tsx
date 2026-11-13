// Pil ömrü kartı (hayvan kartı ve tasma ekranı): pil kapasitesi girilir, beklenen
// ve ölçülen süre yan yana, her birinin neye dayandığı yazar.
import { useMemo, useState, useSyncExternalStore } from "react";
import { CaretRight } from "@phosphor-icons/react";
import { deviceSettings, pilotSetBatteryCapacity } from "../../app/ownerAccountPilotRuntime.ts";
import {
  batteryCapacity, drainRuns, fitDrain, lifeAt, lifeWord, MAH_MAX, MAH_MIN, USABLE, type Reading,
} from "../../app/batteryLife.ts";
import { fmt } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { useBack } from "../../app/nav.ts";
import { Card, Cell, Dialog, KV, Section } from "../../ds/components.tsx";

const INFO =
  `Ortalama tüketim = bekleme akımı + rapor başına maliyet × saatteki rapor sayısı. İki sayı bu tasmanın pille ` +
  `çalışırken ölçülen pil düşüşünden çıkarılır; yeterli ölçüm yoksa başka bir tasmada ölçülen değerler kullanılır ve ` +
  `"tahmin" yazar. Pil kapasitesinin %${USABLE * 100}'i kullanılabilir sayılır. İki ayrı rapor aralığında en az 6'şar saat ` +
  `pille çalışınca iki sayı da ölçülmüş olur. Güneş katkısı katılmaz.`;

/** The report interval the collar is set to (what it reported, else what was asked), not one record's period. */
export function useConfiguredInterval(a: Pick<Animal, "device" | "periodS"> | null, device?: string): number | null {
  const all = useSyncExternalStore((fn) => deviceSettings.subscribe(fn), () => deviceSettings.snapshot());
  const id = (device ?? a?.device ?? "").toUpperCase();
  const e = id ? all[id] : undefined;
  const v = e?.reported?.interval_s ?? e?.wanted?.interval_s;
  return typeof v === "number" && v > 0 ? v : a?.periodS || null;
}

export function BatteryLife({ device, intervalS, series, nowPct }: {
  device: string; intervalS: number | null;
  series: Reading[]; nowPct: number | null;
}) {
  const caps = useSyncExternalStore(batteryCapacity.subscribe, batteryCapacity.snapshot);
  const mAh = caps[device.toUpperCase()] ?? null;
  const [editing, setEditing] = useState(false);
  const drain = useMemo(() => fitDrain(mAh ? drainRuns(series, mAh) : []), [series, mAh]);
  const life = mAh && intervalS ? lifeAt(mAh, intervalS, drain, nowPct) : null;
  const basis = drain.measured === "both" ? `bu tasmanın ${Math.round(drain.hours)} saatlik ölçümünden`
    : drain.measured === "one" ? `kısmen ölçüldü (${Math.round(drain.hours)} saat, tek rapor aralığı)` : "tahmin · bu tasmada henüz ölçülmedi";

  return (
    <>
      <Section title="Pil ömrü" info={INFO} />
      <Card>
        <Cell title="Pil kapasitesi" sub={mAh ? undefined : "Hesap için tasmadaki pilin mAh değerini gir"}
          trail={<span className="sm row" style={{ gap: 4 }}>{mAh ? `${fmt.num(mAh)} mAh` : "Gir"}<CaretRight size={16} color="var(--mute)" /></span>}
          onClick={() => setEditing(true)} />
        {mAh && <>
          {life ? <>
            <KV k={`Dolu pille · ${fmt.period(intervalS!)} aralıkla`}>{lifeWord(life.daysFull)}</KV>
            {life.daysLeft !== null && <KV k="Kalan">{lifeWord(life.daysLeft)}</KV>}
          </> : <KV k="Dolu pille">Rapor aralığı bilinmiyor</KV>}
          <p className="fine" style={{ padding: "8px 16px 12px", margin: 0 }}>
            {life ? `Ortalama ${fmt.dec(life.avgMa, 2)} mA · ` : ""}
            {`bekleme ${fmt.dec(drain.baseMa, 2)} mA, rapor başına ${fmt.dec(drain.packetMah, 2)} mAh · ${basis}.`}
          </p>
        </>}
      </Card>
      {editing && <CapacityDialog device={device} current={mAh} onClose={() => setEditing(false)} />}
    </>
  );
}

function CapacityDialog({ device, current, onClose }: { device: string; current: number | null; onClose: () => void }) {
  const [text, setText] = useState(current ? String(current) : "");
  const [error, setError] = useState("");
  useBack(onClose);
  const save = (v: number | null) => {
    pilotSetBatteryCapacity(device, v).then(onClose,
      (e) => setError(e instanceof Error ? e.message : "Kaydedilemedi."));
  };
  const n = Number(text.trim());
  return (
    <Dialog title="Pil kapasitesi" onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <p className="sm" style={{ margin: 0 }}>Tasmadaki pilin etiketindeki değer (mAh). Hesapta saklanır; bütün telefonlarda aynı görünür. Tasmaya yazılmaz.</p>
        <input className="inp" inputMode="numeric" placeholder="Örn. 3200" value={text} aria-label="Pil kapasitesi (mAh)"
          onChange={(e) => setText(e.target.value.replace(/[^0-9]/g, ""))} />
        <div className="btns" style={{ padding: 0 }}>
          {current !== null && <button className="btn" onClick={() => save(null)}>Sil</button>}
          <button className="btn primary" disabled={!Number.isInteger(n) || n < MAH_MIN || n > MAH_MAX} onClick={() => save(n)}>Kaydet</button>
        </div>
        {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
      </div>
    </Dialog>
  );
}
