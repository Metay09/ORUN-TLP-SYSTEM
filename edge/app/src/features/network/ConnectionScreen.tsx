// Bağlantı: veri bu telefona geliyor mu? Üstte durum (sunucu, gateway,
// kapsama), altında gateway'e Bluetooth / USB ile bağlanma ve gateway'in yeri,
// en altta teknik ayrıntılar. Eşitleme kendiliğinden; düğme yok.
import { useEffect, useRef, useState } from "react";
import { Bluetooth, CaretRight, CellSignalFull, CloudCheck, MapPin, Usb, WifiHigh, X } from "@phosphor-icons/react";
import { coverageSummary } from "../../app/coverage.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import {
  onPilotAccountChange, onPilotObservationSync, pilotAccountStatus, pilotObservationSyncState,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Bar, Card, Cell, DemoFlag, Header, KV, Section, Seg } from "../../ds/components.tsx";
import { startPosition, usePosition } from "../map/position.ts";
import { stationWords } from "./words.ts";

type Via = "ble" | "usb";

/** Sunucu satırı: hesap, son eşitleme, gönderilmeyi bekleyen kayıt. */
function serverLine(signedIn: boolean, sync: ReturnType<typeof pilotObservationSyncState>,
                    pending: number | null, sendError: string | null): string {
  if (!signedIn) return "Hesaba giriş yapılmadı · dokun";
  const parts: string[] = [];
  if (sync.running) parts.push("Eşitleniyor…");
  else if (sync.error) parts.push(sync.error);
  else if (sync.last) parts.push(`Son eşitleme ${fmt.clock(sync.last.at)} · ${sync.last.latest} tasma`);
  else parts.push("Eşitleme bekleniyor…");
  if ((pending ?? 0) > 0) parts.push(`${fmt.num(pending ?? 0)} kayıt gönderilecek`);
  if (sendError) parts.push("Gönderilemedi: " + sendError);
  return parts.join(" · ");
}

export function ConnectionScreen() {
  const m = useApp();
  const nav = useNav();
  const pos = usePosition();
  const st = m.station, s = m.snap, sv = m.server;
  const words = stationWords(st, s);
  const linkOn = st.link !== "none";
  const cov = coverageSummary(m.coverage.cells);
  const held = st.held ?? 0;
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const [sync, setSync] = useState(pilotObservationSyncState);
  const [via, setVia] = useState<Via>("ble");
  const [marking, setMarking] = useState(false);
  useEffect(() => onPilotObservationSync(() => setSync(pilotObservationSyncState())), []);
  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  // "Şu an gateway'in yanındayım": mark as soon as the phone knows where it is.
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  const serverTone = !account || sync.error || sv.error ? "aging" : sync.last ? "fresh" : "";
  return (
    <div className="page">
      <Header title={tr.network.title} />
      {m.demo && <DemoFlag onPage />}

      <Section title={tr.network.chain} />
      <Card>
        <Cell lead={<span className={"ic " + serverTone}><CloudCheck size={22} /></span>}
          title={tr.network.server} wrapSub
          sub={serverLine(Boolean(account), sync, sv.pending, sv.error)}
          trail={!account ? <CaretRight size={18} color="var(--mute)" /> : undefined}
          onClick={!account ? () => nav.push({ to: "settings" }) : undefined} />
        <Cell lead={<span className={"ic " + (words.dot === "ok" ? "fresh" : words.dot === "wait" ? "aging" : "")}><WifiHigh size={22} /></span>}
          title={tr.network.station} wrapSub
          sub={st.detail && st.phase !== "connected" ? `${words.text} · ${st.detail}` : words.text} />
        <Cell lead={<span className="ic"><CellSignalFull size={22} /></span>}
          title={tr.network.coverage} wrapSub
          sub={cov.cells ? tr.network.coverageLine(cov.cells, cov.strong, cov.weak) : tr.network.coverageNone}
          trail={cov.cells ? <CaretRight size={18} color="var(--mute)" /> : undefined}
          onClick={cov.cells ? () => { m.actions.setPrefs({ layers: { ...m.prefs.layers, coverage: true } }); nav.showOnMap(null); } : undefined} />
      </Card>

      <Section title={tr.network.station} />
      <Card>
        {linkOn ? (
          <div className="stack" style={{ padding: 16 }}>
            <span className="sm strong">{words.text}</span>
            {st.capacity !== null && (
              <>
                <div className="row" style={{ justifyContent: "space-between" }}><span className="sm">{tr.station.memory}</span><span className="sm strong">{fmt.num(held)} / {fmt.num(st.capacity)}</span></div>
                <Bar ratio={held / st.capacity} tone={held / st.capacity >= 0.8 ? "bad" : held / st.capacity >= 0.5 ? "warn" : undefined} />
              </>
            )}
            <button className="btn quiet small" style={{ color: "var(--alarm-t)", alignSelf: "flex-start" }} onClick={() => m.source.disconnect()}>{tr.station.disconnect}</button>
          </div>
        ) : (
          <div className="stack" style={{ padding: 16 }}>
            <Seg<Via> value={via} onChange={setVia}
              options={[{ value: "ble", label: tr.station.ble }, { value: "usb", label: tr.station.usb }]} />
            {via === "ble" ? (
              <>
                <button className="btn primary wide" disabled={!s.ready} onClick={() => m.source.connectBle(false)}><Bluetooth size={20} />{tr.station.connectBle}</button>
                <button className="btn quiet wide" disabled={!s.ready} onClick={() => m.source.connectBle(true)}>{tr.station.pickOther}</button>
              </>
            ) : (
              <button className="btn primary wide" disabled={!s.ready} onClick={() => m.source.connectUsb()}><Usb size={20} />{tr.station.connectUsb}</button>
            )}
            <span className="fine">{tr.station.background}</span>
          </div>
        )}
        {st.id && (
          <>
            <KV k={tr.station.position}>{st.position ? fmt.coords(st.position.lat, st.position.lon) : tr.station.positionNone}</KV>
            <div className="stack" style={{ padding: "4px 16px 14px" }}>
              <button className="btn wide" onClick={() => { startPosition(); setMarking(true); }} disabled={marking}>
                <MapPin size={20} />{tr.station.markHere}
              </button>
              {st.position && <button className="btn quiet wide" onClick={() => m.actions.markStation(null)}><X size={18} />{tr.station.clearPosition}</button>}
              <span className="fine">{marking && !pos.pos ? tr.find.noPhone : tr.station.markHelp}</span>
            </div>
          </>
        )}
      </Card>

      <TechDetails />
    </div>
  );
}

function TechDetails() {
  const m = useApp();
  const s = m.snap, c = s.counts, sv = m.server;
  const end = useRef<HTMLDivElement>(null);
  return (
    <details className="tech" onToggle={() => end.current?.scrollIntoView({ block: "end" })}>
      <summary>{tr.common.tech}</summary>
      <Card>
        <KV k={tr.tech.gateway}><code>{s.gateway ?? "—"}</code></KV>
        <KV k={tr.tech.held}>{s.gatewayHeld ?? "—"}</KV>
        <KV k={tr.tech.session}>{c.STORED} / {c.DUPLICATE}</KV>
        <KV k={tr.tech.handoff}>{s.handedOff} / {s.pending}</KV>
        <KV k={tr.tech.rejected}>{c.REJECTED + s.rejected} / {c.CONFLICT} / {c.UNCERTAIN}</KV>
        <KV k={tr.tech.phone}>{s.records ?? "—"}</KV>
        {sv.uploaded !== null && <KV k={tr.tech.server}>{sv.uploaded} / {sv.pending}</KV>}
        {sv.lastOk && <KV k="Son sunucu erişimi">{sv.lastOk}</KV>}
        <KV k={tr.tech.storage}>{s.storage || "—"}</KV>
      </Card>
      <Section title={tr.tech.log} />
      <div className="log" role="log">{s.log.join("\n")}<div ref={end} /></div>
    </details>
  );
}
