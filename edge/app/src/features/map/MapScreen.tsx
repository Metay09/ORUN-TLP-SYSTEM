// Harita: the home screen. Animals with freshness rings over satellite /
// terrain / street, the selected animal's trail with a time slider, density,
// 5 m coverage, the station and the phone's own position; the herd list in a
// bottom sheet.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Compass, CornersOut, Crosshair, CubeFocus, NavigationArrow, Pause, Play, Polygon, Stack, X,
} from "@phosphor-icons/react";
import { hourlyActivity, inRange } from "../../app/activity.ts";
import { bearingDeg, distanceM, type LatLon } from "../../app/geo.ts";
import { attentionLine, doingOf, epochMs, herdSummary, needsLook } from "../../app/herd.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { rangeStart, type Range } from "../../app/stores.ts";
import { readableTrail } from "./routeGeometry.ts";
import { validateRing } from "../../app/zones.ts";
import { AnimalRing, Chips, Dialog, Metric, Strip, toneOfStatus } from "../../ds/components.tsx";
import { LayerPanel } from "./LayerPanel.tsx";
import { ApplyFenceDialog } from "../zones/ApplyFence.tsx";
import { MapEngine, type LinkLine, type MapData } from "./mapEngine.ts";
import { onOfflineMapsChanged, openOfflineMap } from "./offlinePacks/offlineMap.ts";
import { DrawZone, type DrawHooks } from "./DrawZone.tsx";
import { startPosition, usePosition } from "./position.ts";
import { Sheet, type Snap } from "./Sheet.tsx";
import { stationWords } from "../network/words.ts";

let savedView: { center: [number, number]; zoom: number } | undefined;
type TrailRange = Range | "recent";
const RANGES: TrailRange[] = ["recent", "today", "h24", "d3", "d7"];

export function MapScreen({ visible }: { visible: boolean }) {
  const m = useApp();
  const nav = useNav();
  const pos = usePosition();
  const el = useRef<HTMLDivElement>(null);
  const engine = useRef<MapEngine | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [trailRange, setTrailRange] = useState<TrailRange>("recent");
  const [snap, setSnap] = useState<Snap>("peek");
  const [sheetPx, setSheetPx] = useState(150);
  const [panel, setPanel] = useState(false);
  const [offline, setOffline] = useState(false);
  const [zoom, setZoom] = useState(14);
  const [turned, setTurned] = useState(false);   // rotated or tilted: show the north button
  const [win, setWin] = useState<{ from: number; to: number } | null>(null);
  const [cursor, setCursor] = useState<number | null>(null);
  const [playing, setPlaying] = useState(false);
  const [selZone, setSelZone] = useState<string | null>(null);
  const [drawing, setDrawing] = useState<{ zone: string | null; key: number } | null>(null);
  const [tiles, setTiles] = useState(false);
  const [phoneMap, setPhoneMap] = useState(false);   // offline pack shown: no internet
  const [askSend, setAskSend] = useState<string | null>(null);   // 3a: "tasmalara gönder?"
  const [sending, setSending] = useState(false);
  const [flash, setFlash] = useState<string | null>(null);
  const drawHooks = useRef<DrawHooks | null>(null);
  const fitted = useRef(false);
  const tiltRef = useRef(m.prefs.tilt);
  tiltRef.current = m.prefs.tilt;

  const herd = m.herd;
  const a = herd.find((x) => x.id === selected) ?? null;
  // A selected animal starts with a short, readable route. History and
  // explicit time choices can still show the full record range.
  const from = win?.from ?? (a && trailRange === "recent" ? m.now - 2 * 3600_000 : rangeStart(trailRange === "recent" ? m.prefs.range : trailRange, m.now));
  const to = win?.to ?? m.now;
  const L = m.prefs.layers;

  // ---- engine lifecycle ---------------------------------------------------
  useEffect(() => {
    const e = new MapEngine(el.current!, {
      onSelect: (id) => { setSelected(id); setTrailRange("recent"); setCursor(null); setPlaying(false); setSelZone(null); if (!id) { setWin(null); setSnap("peek"); } },
      onOffline: setOffline,
      onTiles: setTiles,
      onOfflinePack: setPhoneMap,
      onDraft: (r) => drawHooks.current?.onDraft(r),
      onDraftTap: (p) => drawHooks.current?.onTap(p),
      onCentre: (p) => drawHooks.current?.onCentre(p),
      onZoom: setZoom,
      onView: (bearing, pitch) => setTurned(Math.abs(bearing) > 2 || (pitch > 2 && !tiltRef.current)),
    }, savedView);
    engine.current = e;
    // 2: the phone's own map pack, drawn when there is no internet.
    let live = true;
    const load = () => {
      const c = e.map.getCenter();
      openOfflineMap(c.lng, c.lat)
        .then((pack) => { if (live) e.setOfflineMap(pack); })
        .catch(() => { if (live) e.setOfflineMap(null); });
    };
    load();
    const off = onOfflineMapsChanged(load);
    return () => { live = false; off(); savedView = e.view(); e.destroy(); engine.current = null; };
  }, []);
  useEffect(() => { if (visible) engine.current?.map.resize(); }, [visible]);
  const sat = m.prefs.sat;
  useEffect(() => { void engine.current?.setBase(m.prefs.base, L, sat); }, [m.prefs.base, L.labels, sat.kind, sat.release, sat.maxZoom]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { engine.current?.setZoneFill(m.prefs.zoneFill); }, [m.prefs.zoneFill]);
  useEffect(() => { if (!flash) return; const t = setTimeout(() => setFlash(null), 4000); return () => clearTimeout(t); }, [flash]);
  useEffect(() => { engine.current?.setLayers(L); }, [L]);
  useEffect(() => { engine.current?.setTilt(m.prefs.tilt); }, [m.prefs.tilt]);
  useEffect(() => { engine.current?.setPadding(sheetPx, 140, drawing !== null); }, [sheetPx, drawing]);

  // ---- data ---------------------------------------------------------------
  const trailPts = useMemo(() => (a ? inRange(a.points, from, to) : []), [a, from, to]);
  const trailBreaks = useMemo(() => readableTrail(trailPts).breaks, [trailPts]);
  const heatPts = useMemo(() => (L.heat ? (a ? [a] : herd).flatMap((x) => inRange(x.points, from, to)) : []), [L.heat, a, herd, from, to]);
  const zoneViews = useMemo(() => m.zones.map((z) => ({ id: z.id, name: z.name, ring: z.ring, active: z.active, valid: !validateRing(z.ring) })), [m.zones]);
  const herdTrails = useMemo(() => (L.herdTrails && !a ? herd.filter((x) => x.last).map((x) => ({ id: x.id, color: x.color, points: inRange(x.points, from, to) })) : []),
    [L.herdTrails, a, herd, from, to]);
  const links = useMemo<LinkLine[]>(() => {
    const sp = m.station.position;
    if (!L.links || !sp) return [];
    return herd.filter((x) => x.last && x.last.rssi !== null && (x.freshness === "fresh" || x.freshness === "aging"))
      .map((x) => ({ from: x.last!, to: sp, rssi: x.last!.rssi! }));
  }, [L.links, herd, m.station.position]);
  const data = useMemo<MapData>(() => {
    return {
      herd, selected,
      trail: a ? { points: trailPts, color: a.color } : null,
      cursor,
      follow: playing,
      heat: heatPts,
      coverage: L.coverage ? m.coverage.cells : [],
      stations: m.station.position && m.station.id ? [{ id: m.station.id, pos: m.station.position, connected: m.station.phase === "connected" }] : [],
      me: pos.pos,
      zones: zoneViews,
      selectedZone: selZone,
      herdTrails,
      links,
    };
  }, [herd, selected, a, trailPts, cursor, playing, heatPts, L.coverage, m.coverage, m.station, pos.pos, zoneViews, selZone, herdTrails, links]);
  useEffect(() => { engine.current?.setData(data); }, [data]);

  // First data: frame the herd (or the station).
  useEffect(() => {
    if (fitted.current || savedView) return;
    const located = herd.filter((x) => x.last).map((x) => x.last!);
    if (located.length) { fitted.current = true; engine.current?.fit(located); }
    else if (m.station.position) { fitted.current = true; engine.current?.flyTo(m.station.position, 15); }
  }, [herd, m.station.position]);

  // Another screen asked to show an animal (or a day of it) here.
  useEffect(() => {
    if (!nav.focus.key) return;
    setSelected(nav.focus.selected);
    setTrailRange("recent");
    setWin(nav.focus.window);
    setCursor(nav.focus.window ? nav.focus.window.from : null);
    setPlaying(false);
    setSnap(nav.focus.selected ? "half" : "peek");
    setSelZone(nav.focus.draw ? null : nav.focus.zone);
    setDrawing(nav.focus.draw ? { zone: nav.focus.draw.zone, key: nav.focus.key } : null);
    if (nav.focus.zone && !nav.focus.draw) {
      fitted.current = true;
      const z = m.zones.find((x) => x.id === nav.focus.zone);
      if (z) engine.current?.fitRing(z.ring);
    }
  }, [nav.focus]); // eslint-disable-line react-hooks/exhaustive-deps

  // Selecting an animal brings it into view.
  useEffect(() => {
    if (!a?.last) return;
    if (win) {
      const pts = inRange(a.points, win.from, win.to);
      if (pts.length) { engine.current?.fit(pts, 17.5); return; }
    }
    engine.current?.flyTo(a.last);
    if (snap === "peek") setSnap("half");
  }, [selected, win]); // eslint-disable-line react-hooks/exhaustive-deps

  // Playback: ~18 s across the shown period.
  useEffect(() => {
    if (!playing || !trailPts.length) return;
    const t0 = epochMs(trailPts[0].time), t1 = epochMs(trailPts[trailPts.length - 1].time);
    const step = Math.max(60_000, (t1 - t0) / 150);
    const t = setInterval(() => setCursor((c) => {
      const next = (c ?? t0) + step;
      if (next >= t1) { setPlaying(false); return t1; }
      return next;
    }), 120);
    return () => clearInterval(t);
  }, [playing, trailPts]);

  // "Konumum": centre now if known, else as soon as the first fix comes.
  const wantCentre = useRef(false);
  const locate = useCallback(() => {
    startPosition();
    if (pos.pos) { wantCentre.current = false; engine.current?.flyTo(pos.pos, Math.max(engine.current.map.getZoom(), 16)); }
    else wantCentre.current = true;
  }, [pos.pos]);
  useEffect(() => {
    if (pos.pos && wantCentre.current) { wantCentre.current = false; engine.current?.flyTo(pos.pos, 16); }
  }, [pos.pos]);

  const clear = () => { setSelected(null); setTrailRange("recent"); setWin(null); setCursor(null); setPlaying(false); setSnap("peek"); };
  // Hidden map controls must not consume Android Back while a pushed screen is open.
  useBack(() => setPanel(false), visible && nav.stack.length === 0 && panel);
  useBack(() => { if (selected) clear(); else if (selZone) setSelZone(null); else setSnap("peek"); },
    visible && nav.stack.length === 0 && !drawing && (selected !== null || selZone !== null || snap !== "peek"));

  const look = herd.filter(needsLook);
  const lastArrival = m.devices.reduce<number | null>((x, d) => (d.lastArrival !== null && (x === null || d.lastArrival > x) ? d.lastArrival : x), null);
  const st = stationWords(m.station, m.snap, lastArrival);
  const me = pos.pos;

  const drawZone = drawing ? m.zones.find((z) => z.id === drawing.zone) ?? null : null;

  return (
    <section className="map-screen" hidden={!visible}>
      {askSend && (
        <Dialog title="Alan kaydedildi" onClose={() => setAskSend(null)}>
          <div className="stack" style={{ padding: "0 20px 16px" }}>
            <p className="sm" style={{ margin: 0 }}>"{askSend}" tasmalara uygulansın mı? Kaydetmek tasmadaki alanı değiştirmez; uygulayınca LoRa'dan ya da tasmaya bağlanınca işlenir. Sonra Alanlar → Tasmalara uygula'dan da yapabilirsin.</p>
            <div className="btns">
              <button className="btn" onClick={() => setAskSend(null)}>Sonra</button>
              <button className="btn primary" onClick={() => { setAskSend(null); setSending(true); }}>Uygula</button>
            </div>
          </div>
        </Dialog>
      )}
      {sending && <ApplyFenceDialog onClose={() => setSending(false)} />}
      <div ref={el} className="map-canvas" />
      {drawing ? (
        <DrawZone key={drawing.key} engine={engine.current} zone={drawZone} hooks={drawHooks}
          onDone={(z) => {
            setDrawing(null);
            if (!z) return;
            setSelZone(z.id); setFlash(tr.zones.saved(z.name));
            // 3a: yeni alan önce bütün tasmalara (sonra değiştirilebilir); kaydedince sor.
            try { if (!m.targetBook.rules[z.id]) m.actions.saveTargetRule(z.id, { scope: "all", devices: [], groups: [] }); }
            catch { /* hedefler okunamıyorsa dokunma */ }
            setAskSend(z.name);
          }} />
      ) : <>
      <div className="map-top">
        <button className="me-btn" onClick={() => nav.push({ to: "zones" })} aria-label={tr.zones.title}><Polygon size={24} /><span>{tr.zones.title}</span></button>
      </div>
      <div className="map-status">
        <button className="status-chip" onClick={() => nav.setTab("network")}>
          <span className={"dot " + st.dot} /><span className="ellip">{st.short}</span>
        </button>
        {offline && <span className="status-chip note">{tr.states.offlineMap}</span>}
        {phoneMap && <span className="status-chip note">{tr.map.phoneMap}</span>}
        {tiles && !offline && !phoneMap && <span className="status-chip note">{navigator.onLine ? tr.map.tilesTrouble : tr.map.tilesOffline}</span>}
        {flash && <span className="status-chip">{flash}</span>}
        {L.herdTrails && !a && <button className="status-chip note" onClick={() => m.actions.setPrefs({ layers: { ...L, herdTrails: false } })}>Sürü izlerini gizle</button>}
        {L.coverage && zoom < 14 && <span className="status-chip note">{tr.map.zoomForCoverage}</span>}
      </div>
      <div className="map-tools">
        <button className="fab" onClick={() => setPanel(true)} aria-label={tr.map.layers}><Stack size={24} /></button>
        <button className="fab" onClick={locate} aria-label={tr.map.myLocation}><Crosshair size={24} /></button>
        <button className="fab" onClick={() => engine.current?.fit(herd.filter((x) => x.last).map((x) => x.last!))} aria-label={tr.map.fitHerd}><CornersOut size={24} /></button>
        <button className={"fab" + (m.prefs.tilt ? " on" : "")} onClick={() => m.actions.setPrefs({ tilt: !m.prefs.tilt })} aria-label={tr.map.tilt} aria-pressed={m.prefs.tilt}><CubeFocus size={24} /></button>
        {turned && <button className="fab" onClick={() => engine.current?.resetNorth()} aria-label={tr.map.north}><Compass size={24} /></button>}
      </div>

      <Sheet snap={snap} peek={a ? 140 : herd.length ? 150 : 290} half={a ? 0.62 : 0.46} onSnap={setSnap} onHeight={setSheetPx}
        header={a ? null : (
          <div className="sum">
            <div className="disp ellip">{herdSummary(herd)}</div>
            {look.length > 0 && <div className="sm ellip">{attentionLine(herd, m.now)}</div>}
          </div>
        )}>
        {a ? (
          <Selected a={a} me={me} now={m.now} pts={trailPts} breaks={trailBreaks} cursor={cursor} playing={playing} range={trailRange} win={win}
            onClose={clear} onFind={() => nav.push({ to: "find", id: a.id })} onProfile={() => nav.push({ to: "animal", id: a.id })}
            onCursor={(c) => { setPlaying(false); setCursor(c); }} onPlay={() => {
              if (!trailPts.length) return;
              const t1 = epochMs(trailPts[trailPts.length - 1].time);
              if (cursor === null || cursor >= t1) setCursor(epochMs(trailPts[0].time));
              setPlaying((p) => !p);
            }}
            onRange={(r) => { setWin(null); setCursor(null); setPlaying(false); setTrailRange(r); if (r !== "recent") m.actions.setPrefs({ range: r }); }} />
        ) : (
          <>
            {look.length > 0 && (
              <div className="att">
                {look.slice(0, 6).map((x) => (
                  <button key={x.id} className={"att-chip " + toneOfStatus(x.status)} onClick={() => { setSelected(x.id); setTrailRange("recent"); }}>
                    <AnimalRing a={x} size={36} />
                    <span className="nm"><b>{x.name}</b><span className={`t-${toneOfStatus(x.status)}`}>{x.short}</span></span>
                  </button>
                ))}
              </div>
            )}
            {herd.length ? <HerdRows herd={herd} me={me} now={m.now} onPick={(id) => { setSelected(id); setTrailRange("recent"); }} /> : (
              <div className="stack" style={{ padding: "8px 20px 16px" }}>
                <span className="sm">{tr.states.noAnimalsBody}</span>
                <button className="btn primary wide" onClick={() => nav.setTab("network")}>{tr.states.connect}</button>
              </div>
            )}
          </>
        )}
      </Sheet>

      </>}
      {panel && !drawing && <LayerPanel onClose={() => setPanel(false)} />}
    </section>
  );
}

export function HerdRows({ herd, me, now, onPick }: { herd: Animal[]; me: LatLon | null; now: number; onPick: (id: string) => void }) {
  return (
    <div>
      {herd.map((a) => (
        <button key={a.id} className="cell" onClick={() => onPick(a.id)}>
          <AnimalRing a={a} />
          <span className="cm">
            <span className="ct">{a.name}</span>
            <span className={`cs strong t-${toneOfStatus(a.status)}`}>{a.seen}</span>
            {a.reports.some((p) => p.active_s !== null) && <span style={{ marginTop: 4 }}><Strip values={hourlyActivity(a.reports, now)} /></span>}
          </span>
          {me && a.last && <span className="cap">{fmt.distance(distanceM(me, a.last))}</span>}
        </button>
      ))}
    </div>
  );
}

function Selected({ a, me, now, pts, breaks, cursor, playing, range, win, onClose, onFind, onProfile, onCursor, onPlay, onRange }: {
  a: Animal; me: LatLon | null; now: number; pts: Animal["points"]; breaks: number; cursor: number | null; playing: boolean;
  range: TrailRange; win: { from: number; to: number } | null;
  onClose: () => void; onFind: () => void; onProfile: () => void;
  onCursor: (c: number) => void; onPlay: () => void; onRange: (r: TrailRange) => void;
}) {
  const tone = toneOfStatus(a.status);
  useEffect(() => { startPosition(); }, []);   // distance needs the phone's position
  const d = me && a.last ? distanceM(me, a.last) : null;
  const doing = doingOf(a.activeShare);
  const t0 = pts.length ? epochMs(pts[0].time) : 0, t1 = pts.length ? epochMs(pts[pts.length - 1].time) : 0;
  let idx = pts.length;
  if (cursor !== null) { idx = 0; for (const p of pts) { if (epochMs(p.time) <= cursor) idx++; else break; } }
  return (
    <div className="sel">
      <div className="sel-head">
        <AnimalRing a={a} size={56} />
        <div className="cm">
          <span className="disp ellip">{a.name}</span>
          <span className={`strong t-${tone}`} style={{ fontSize: 15 }}>{a.seen}</span>
          {a.zone && <span className="cap ellip">{tr.map.inZone(a.zone)}</span>}
        </div>
        <button className="circ" onClick={onClose} aria-label={tr.common.close}><X size={20} /></button>
      </div>
      <div className="row">
        <Metric label={tr.map.distance} value={d !== null ? (d < 25 ? tr.find.near : fmt.distance(d)) : "—"}
          sub={d !== null && d >= 25 && me && a.last ? cap(fmt.direction(bearingDeg(me, a.last))) : d === null ? tr.find.noPhone : ""} />
        <Metric label={tr.map.now} value={doing ? tr.map.doing[doing] : batteryText(a)} sub={doing ? batteryText(a) : a.last ? fmt.clock(epochMs(a.last.time)) : ""} />
      </div>
      <div className="row">
        <button className="btn signal grow" onClick={onFind} disabled={!a.last}><NavigationArrow size={20} weight="fill" />{tr.animal.find}</button>
        <button className="btn grow" onClick={onProfile}>{tr.map.profile}</button>
      </div>
      <div className="timebar">
        {!win && <Chips<TrailRange> value={range} onChange={onRange} options={RANGES.map((r) => ({ value: r, label: r === "recent" ? "2 saat" : tr.map.range[r] }))} />}
        {win && <span className="sm strong">{fmt.day(win.from, now)}</span>}
        <span className="fine">{range === "recent" && !win ? "Son 2 saatin konum izi." : "Seçilen dönemin konum izi."} Başlangıç: beyaz nokta · son konum: hayvan işareti.</span>
        {breaks > 0 && <span className="fine">{breaks} iz kesintisi var; eksik konumların arasına çizgi çekilmedi.</span>}
        {pts.length > 1 ? (
          <>
            <div className="row">
              <button className="circ" onClick={onPlay} aria-label={playing ? tr.map.pause : tr.map.play} style={{ background: "var(--card)" }}>
                {playing ? <Pause size={20} weight="fill" /> : <Play size={20} weight="fill" />}
              </button>
              <input type="range" min={t0} max={t1} step={60_000} value={cursor ?? t1} aria-label={tr.map.range.title}
                onChange={(e) => onCursor(Number(e.target.value))} />
            </div>
            <span className="cap">{fmt.clock(cursor ?? t1)} · {fmt.dateShort(cursor ?? t1)} · {tr.map.recordOf(Math.max(1, idx), pts.length)}</span>
          </>
        ) : <span className="cap">{tr.map.noTrail}</span>}
      </div>
    </div>
  );
}

function batteryText(a: Animal): string {
  if (a.battery === "critical") return tr.status.batteryCritical;
  if (a.battery === "low") return tr.status.batteryLow + (a.batteryPercent !== null ? ` · %${a.batteryPercent}` : "");
  if (a.batteryPercent !== null) return `Pil %${a.batteryPercent}`;
  return "—";
}
