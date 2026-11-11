// Pil ömrü kartı (hayvan kartı ve tasma ekranı): pil kapasitesi girilir, beklenen
// ve ölçülen süre yan yana, her birinin neye dayandığı yazar.
import { useMemo, useState, useSyncExternalStore } from "react";
import { CaretRight } from "@phosphor-icons/react";
import { batteryOutlook, smoothBattery } from "../../app/activity.ts";
import { deviceSettings, pilotSetBatteryCapacity } from "../../app/ownerAccountPilotRuntime.ts";
import {
  batteryCapacity, expectedLife, lifeWord, MAH_MAX, MAH_MIN, measuredLife, PUBLISHED, USABLE,
} from "../../app/batteryLife.ts";
import { batteryPercent } from "../../app/herd.ts";
import { fmt } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { useBack } from "../../app/nav.ts";
import { Card, Cell, Dialog, KV, Section } from "../../ds/components.tsx";

const INFO =
  `Beklenen: pil kapasitesinin %${USABLE * 100}'i, rapor aralığı ve üreticilerin yayımladığı akımlarla hesaplanır ` +
  `(GPS ${PUBLISHED.gnssMa} mA, her raporda ~${PUBLISHED.gnssOnS} sn; telsiz gönderirken ${PUBLISHED.txMa} mA, ` +
  `rapordan sonra ${PUBLISHED.rxAfterReportS} sn ${PUBLISHED.rxMa} mA dinleme). Bugünkü tasma yazılımında işlemci ` +
  `uyumuyor (taban ~${PUBLISHED.baseAwakeMa} mA); gerçek uyku gelince (Y3) taban ~${PUBLISHED.baseSleepMa * 1000} µA olur. ` +
  `Ölçülen: sahada pilin günlük düşüşünden; USB'deki ya da güneşle dengede olan tasmada ölçülemez. Güneş katkısı katılmaz.`;

/** The report interval the collar is set to (what it reported, else what was asked), not one record's period. */
export function useConfiguredInterval(a: Pick<Animal, "device" | "periodS"> | null, device?: string): number | null {
  const all = useSyncExternalStore((fn) => deviceSettings.subscribe(fn), () => deviceSettings.snapshot());
  const id = (device ?? a?.device ?? "").toUpperCase();
  const e = id ? all[id] : undefined;
  const v = e?.reported?.interval_s ?? e?.wanted?.interval_s;
  return typeof v === "number" && v > 0 ? v : a?.periodS || null;
}

export function BatteryLife({ device, intervalS, series, nowPct }: {
  device: string; intervalS: number | null; series: { t: number; mv: number }[]; nowPct: number | null;
}) {
  const caps = useSyncExternalStore(batteryCapacity.subscribe, batteryCapacity.snapshot);
  const mAh = caps[device.toUpperCase()] ?? null;
  const [editing, setEditing] = useState(false);
  const now = Date.now();
  const outlook = useMemo(() => {
    const pct = smoothBattery(series.map((b) => ({ t: b.t, p: batteryPercent(b.mv) }))
      .filter((x): x is { t: number; p: number } => x.p !== null));
    return batteryOutlook(pct, now);
  }, [series]); // eslint-disable-line react-hooks/exhaustive-deps

  const awake = mAh && intervalS ? expectedLife(mAh, intervalS, false) : null;
  const sleep = mAh && intervalS ? expectedLife(mAh, intervalS, true) : null;
  const measured = mAh ? measuredLife(mAh, outlook.perDay, nowPct) : null;

  return (
    <>
      <Section title="Pil ömrü" info={INFO} />
      <Card>
        <Cell title="Pil kapasitesi" sub={mAh ? undefined : "Hesap için tasmadaki pilin mAh değerini gir"}
          trail={<span className="sm row" style={{ gap: 4 }}>{mAh ? `${fmt.num(mAh)} mAh` : "Gir"}<CaretRight size={16} color="var(--mute)" /></span>}
          onClick={() => setEditing(true)} />
        {mAh && <>
          {intervalS ? <>
            <KV k="Beklenen · bugünkü yazılım">{lifeWord(awake!.days)}</KV>
            <KV k="Beklenen · gerçek uykuyla">{lifeWord(sleep!.days)}</KV>
          </> : <KV k="Beklenen">Rapor aralığı bilinmiyor</KV>}
          <KV k="Ölçülen">{measured
            ? `${lifeWord(measured.daysFull)} dolu pille${measured.daysLeft !== null ? ` · kalan ${lifeWord(measured.daysLeft)}` : ""}`
            : "Pil düşmüyor"}</KV>
          <p className="fine" style={{ padding: "8px 16px 12px", margin: 0 }}>
            {intervalS ? `Rapor aralığı ${fmt.period(intervalS)} · ortalama ${fmt.dec(awake!.avgMa, 1)} mA bugün, ${fmt.dec(sleep!.avgMa, 2)} mA uykuyla. ` : ""}
            {measured ? `Ölçülen tüketim günde ~${Math.round(measured.mAhPerDay)} mAh (${fmt.dec(measured.avgMa, 1)} mA).`
              : "Ölçülen süre, pille çalışan tasmada birkaç gün sonra çıkar (USB'de ya da güneşle dengedeyken çıkmaz)."}
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
