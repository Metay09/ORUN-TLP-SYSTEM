// Cihaz ayrıntısı: one ORUN device — its services, signal, last contact,
// battery and what it is attached to.
import { CaretRight, CellSignalFull } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Badge, Card, Cell, Empty, KV, Screen, Section } from "../../ds/components.tsx";

export function DeviceScreen({ id }: { id: string }) {
  const m = useApp();
  const nav = useNav();
  useBack(() => nav.pop());
  const d = m.devices.find((x) => x.id === id);
  if (!d) return <Screen title="" onBack={() => nav.pop()}><Empty icon={<CellSignalFull size={30} />} title={tr.herd.noMatch} /></Screen>;
  const a = m.herd.find((x) => x.id === d.asset) ?? null;
  return (
    <Screen title={tr.device.title(d.short)} onBack={() => nav.pop()}>
      {a && (
        <>
          <Section title={tr.device.attached} />
          <Card>
            <Cell lead={<AnimalRing a={a} size={44} />} title={a.name} sub={tr.device.install.animal}
              trail={<CaretRight size={18} color="var(--mute)" />} onClick={() => nav.push({ to: "animal", id: a.id })} />
          </Card>
        </>
      )}
      <Section title={tr.device.services} />
      <Card>
        {d.services.map((s) => (
          <KV key={s.key} k={tr.device.service[s.key]}>
            <Badge tone={s.state === "running" ? "fresh" : s.state === "silent" ? "silent" : "silent"}>{tr.device.serviceState[s.state]}</Badge>
          </KV>
        ))}
      </Card>
      <Section title={tr.device.signal} />
      <Card>
        <KV k={tr.device.signal}>{d.rssi !== null ? tr.device.signalLine(d.rssi, d.snr) : tr.device.signalNone}</KV>
        {d.link && <KV k="Yol">{tr.device.link[d.link]}</KV>}
        <KV k={tr.device.lastContact}>{d.lastTime ? `${fmt.clock(d.lastTime)} · ${fmt.ago(d.lastTime, m.now)}` : "—"}</KV>
        {d.lastArrival !== null && <KV k={tr.animal.arrived}>{fmt.dateTime(d.lastArrival)}</KV>}
        {d.medianDelayS !== null && <KV k={tr.device.delay}>{d.medianDelayS < 60 ? `${d.medianDelayS} sn` : fmt.duration(d.medianDelayS * 1000)}</KV>}
      </Card>
      <Section title={tr.animal.battery} />
      <Card>
        <KV k={tr.animal.battery}>{d.batteryPercent !== null ? `%${d.batteryPercent}${d.batteryMv ? ` · ${fmt.dec(d.batteryMv / 1000, 2)} V` : ""}` : "—"}</KV>
        <KV k={tr.device.report}>{d.periodS ? fmt.period(d.periodS) : "—"}</KV>
        <KV k={tr.device.records}>{fmt.num(d.records)}</KV>
      </Card>
      <details className="tech">
        <summary>{tr.common.tech}</summary>
        <Card><KV k="Kimlik"><code>{d.id}</code></KV></Card>
      </details>
    </Screen>
  );
}
