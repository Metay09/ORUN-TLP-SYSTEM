// Ağ: is every record safe (collar -> station -> phone -> server), the
// station link, devices, coverage, the server, and the technical details.
import { useEffect, useRef, useState, type ReactNode } from "react";
import { Bluetooth, CaretRight, CellSignalFull, CloudCheck, DeviceMobile, MapPin, Usb, WifiHigh, X } from "@phosphor-icons/react";
import { coverageSummary } from "../../app/coverage.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { Bar, Card, Cell, DemoFlag, Header, KV, Section, Toggle } from "../../ds/components.tsx";
import { startPosition, usePosition } from "../map/position.ts";
import { serverWords, stationWords } from "./words.ts";

export function NetworkScreen() {
  const m = useApp();
  const nav = useNav();
  const pos = usePosition();
  const st = m.station, sv = m.server, s = m.snap;
  const words = stationWords(st, s);
  const sw = serverWords(sv);
  const fresh = m.herd.filter((a) => a.freshness === "fresh").length;
  const linkOn = st.link !== "none";
  const cov = coverageSummary(m.coverage.cells);
  const held = st.held ?? 0;
  // "Şu an istasyonun yanındayım": mark as soon as the phone knows where it is.
  const [marking, setMarking] = useState(false);
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  return (
    <div className="page">
      <Header title={tr.network.title} />
      {m.demo && <DemoFlag onPage />}

      <Section title={tr.network.chain} />
      <Card>
        <div className="chain">
          <Node icon={<CellSignalFull size={22} />} tone={fresh ? "fresh" : ""} title={tr.network.nodes.collars} sub={tr.network.collarsLine(m.devices.length, fresh)} />
          <span className={"wire " + (st.phase === "connected" ? "ok" : linkOn ? "wait" : "")} />
          <Node icon={<WifiHigh size={22} />} tone={st.phase === "connected" ? "fresh" : linkOn ? "aging" : ""} title={tr.network.nodes.station}
            sub={st.phase === "connected" ? tr.network.stationHeld(held) : tr.network.stationOff} />
          <span className={"wire " + (st.phase === "connected" ? "ok" : "")} />
          <Node icon={<DeviceMobile size={22} />} tone="fresh" title={tr.network.nodes.phone} sub={tr.network.phoneLine(s.records ?? 0)} />
          <span className={"wire " + (sw.tone === "ok" ? "ok" : sw.tone === "none" ? "" : "wait")} />
          <Node icon={<CloudCheck size={22} />} tone={sw.tone === "ok" ? "fresh" : sw.tone === "none" ? "" : "aging"} title={tr.network.nodes.server}
            sub={sv.configured ? tr.network.serverLine(sv.pending ?? 0) : tr.network.serverOff} />
        </div>
        <p className="fine" style={{ padding: "4px 16px 14px", margin: 0 }}>{tr.network.chainHelp}</p>
      </Card>

      <Section title={tr.network.station} />
      <Card>
        <Cell lead={<span className={"ic " + (words.dot === "ok" ? "fresh" : words.dot === "wait" ? "aging" : "")}><WifiHigh size={22} /></span>}
          title={words.text} sub={st.detail && st.phase !== "connected" ? st.detail : undefined} />
        {st.capacity !== null && (
          <div style={{ padding: "4px 16px 14px" }} className="stack">
            <div className="row" style={{ justifyContent: "space-between" }}><span className="sm">{tr.station.memory}</span><span className="sm strong">{fmt.num(held)} / {fmt.num(st.capacity)}</span></div>
            <Bar ratio={held / st.capacity} tone={held / st.capacity >= 0.8 ? "bad" : held / st.capacity >= 0.5 ? "warn" : undefined} />
          </div>
        )}
        <div className="stack" style={{ padding: "4px 16px 14px" }}>
          {!linkOn && (
            <>
              <button className="btn primary wide" disabled={!s.ready} onClick={() => m.source.connectBle(false)}><Bluetooth size={20} />{tr.station.connectBle}</button>
              <button className="btn wide" disabled={!s.ready} onClick={() => m.source.connectUsb()}><Usb size={20} />{tr.station.connectUsb}</button>
              <button className="btn quiet wide" disabled={!s.ready} onClick={() => m.source.connectBle(true)}>{tr.station.pickOther}</button>
            </>
          )}
          <span className="fine">{tr.station.background}</span>
        </div>
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
        {linkOn && (
          <div style={{ padding: "0 16px 14px" }}>
            <button className="btn quiet small" style={{ color: "var(--alarm-t)" }} onClick={() => m.source.disconnect()}>{tr.station.disconnect}</button>
          </div>
        )}
      </Card>

      <Section title={tr.network.devices} />
      {m.devices.length ? (
        <Card>
          {m.devices.map((d) => {
            const a = m.herd.find((x) => x.id === d.asset);
            return (
              <Cell key={d.id}
                lead={<span className={"ic " + (d.link === "live" ? "fresh" : d.link === "cut" ? "aging" : "")}><CellSignalFull size={20} /></span>}
                title={`${tr.device.title(d.short)}${a?.named ? ` · ${a.name}` : ""}`}
                sub={[d.lastTime ? fmt.ago(d.lastTime, m.now) : "", d.rssi !== null ? `${d.rssi} dBm` : "", d.batteryPercent !== null ? `pil %${d.batteryPercent}` : ""].filter(Boolean).join(" · ")}
                trail={<CaretRight size={18} color="var(--mute)" />} onClick={() => nav.push({ to: "device", id: d.id })} />
            );
          })}
        </Card>
      ) : <p className="sm lead">{tr.network.devicesNone}</p>}

      <Section title={tr.network.coverage} action={cov.cells ? tr.common.showOnMap : undefined}
        onAction={() => { m.actions.setPrefs({ layers: { ...m.prefs.layers, coverage: true } }); nav.showOnMap(null); }} />
      <Card pad><span className="sm">{cov.cells ? tr.network.coverageLine(cov.cells, cov.strong, cov.weak) : tr.network.coverageNone}</span></Card>

      <Section title={tr.network.server} />
      <ServerCard />

      <TechDetails />
    </div>
  );
}

function Node({ icon, tone, title, sub }: { icon: ReactNode; tone: string; title: string; sub: string }) {
  return (
    <div className="node">
      <span className={"nd ic " + tone}>{icon}</span>
      <b>{title}</b>
      <span>{sub}</span>
    </div>
  );
}

function ServerCard() {
  const m = useApp();
  const sv = m.server;
  const [url, setUrl] = useState(sv.url);
  const [token, setToken] = useState(sv.token);
  const [auto, setAuto] = useState(sv.auto);
  const dirty = url !== sv.url || token !== sv.token || auto !== sv.auto;
  const words = serverWords(sv);
  return (
    <Card>
      <p className="sm" style={{ padding: "14px 16px 4px", margin: 0 }}>{words.text}</p>
      <div style={{ paddingTop: 10 }}>
        <label className="lab">{tr.server.address}
          <input className="inp" type="url" inputMode="url" autoCapitalize="off" autoCorrect="off" disabled={m.demo}
            placeholder="http://100.114.36.9:8090" value={url} onChange={(e) => setUrl(e.target.value)} />
        </label>
        <label className="lab">{tr.server.key}
          <input className="inp" type="password" autoCapitalize="off" autoCorrect="off" disabled={m.demo} value={token} onChange={(e) => setToken(e.target.value)} />
        </label>
      </div>
      <Cell title={tr.server.auto} trail={<Toggle on={auto} onChange={setAuto} label={tr.server.auto} />} />
      <div className="btns">
        <button className="btn" disabled={!dirty || m.demo} onClick={() => m.source.saveServer(url, token, auto)}>{tr.common.save}</button>
        <button className="btn primary" disabled={!sv.configured || sv.busy} onClick={() => m.source.sync(true)}>{sv.busy ? tr.server.sending : tr.server.sendNow}</button>
      </div>
      {sv.error && <p className="sm t-aging" style={{ padding: "0 16px 14px", margin: 0 }}>{tr.server.lastError(sv.error)}</p>}
    </Card>
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
        <KV k={tr.tech.storage}>{s.storage || "—"}</KV>
      </Card>
      <Section title={tr.tech.log} />
      <div className="log" role="log">{s.log.join("\n")}<div ref={end} /></div>
    </details>
  );
}
