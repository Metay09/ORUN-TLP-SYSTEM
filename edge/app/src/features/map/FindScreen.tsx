// Bul: walk to an animal. Big distance, an arrow towards its last measured
// position (turned by the phone's compass when it has one), the height
// difference from the two GNSS heights, and how old that position is.
import { useEffect } from "react";
import { CaretLeft, Clock, Crosshair } from "@phosphor-icons/react";
import { bearingDeg, distanceM } from "../../app/geo.ts";
import { epochMs } from "../../app/herd.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { startPosition, usePosition } from "./position.ts";

export function openDirections(lat: number, lon: number, name: string): void {
  // Capacitor hands non-web URLs to Android: geo: opens the phone's maps app.
  window.location.href = `geo:${lat},${lon}?q=${lat},${lon}(${encodeURIComponent(name)})`;
}

export function FindScreen({ id }: { id: string }) {
  const m = useApp();
  const nav = useNav();
  const pos = usePosition();
  useEffect(() => { startPosition(); }, []);
  useBack(() => nav.pop());
  const a = m.herd.find((x) => x.id === id);
  const p = a?.last ?? null;
  const me = pos.pos;

  const d = me && p ? distanceM(me, p) : null;
  const b = me && p ? bearingDeg(me, p) : null;
  const h = pos.compass && me?.heading !== null && me?.heading !== undefined ? me.heading : null;
  const needle = b === null ? 0 : h === null ? b : b - h;
  const rose = h === null ? 0 : -h;
  const t = p ? epochMs(p.time) : null;
  const old = t !== null && m.now - t > 3 * 60_000;
  let height: string | null = null;
  if (p?.alt_m !== null && p?.alt_m !== undefined && pos.altitude !== null && (pos.altAccuracy ?? 0) <= 30) {
    const diff = p.alt_m - pos.altitude;
    height = Math.abs(diff) < 5 ? tr.find.level : diff > 0 ? tr.find.up(diff) : tr.find.down(-diff);
  }
  const [num, unit] = d !== null ? fmt.distanceParts(d) : ["—", ""];

  return (
    <div className="bul" role="dialog" aria-label={a ? tr.find.title(a.name) : tr.animal.find}>
      <div className="hdr">
        <button className="circ" onClick={() => nav.pop()} aria-label={tr.common.back}><CaretLeft size={22} weight="bold" /></button>
        <span className="ttl ellip">{a ? tr.find.title(a.name) : ""}</span>
        <span style={{ width: 44 }} />
      </div>
      {!p ? <p className="muted" style={{ paddingTop: 80 }}>{tr.find.noPosition}</p> : (
        <>
          <div className="compass">
            <svg viewBox="0 0 300 300" aria-hidden="true">
              <g style={{ transform: `rotate(${rose}deg)`, transformOrigin: "150px 150px", transition: "transform .25s ease-out" }}>
                <circle cx="150" cy="150" r="140" fill="none" stroke="rgba(234,243,236,.14)" strokeWidth="2" />
                <circle cx="150" cy="150" r="104" fill="none" stroke="rgba(234,243,236,.08)" strokeWidth="1" />
                <g stroke="rgba(234,243,236,.35)" strokeWidth="2" strokeLinecap="round">
                  {Array.from({ length: 8 }, (_, i) => {
                    const r = (i * Math.PI) / 4, x1 = 150 + 136 * Math.sin(r), y1 = 150 - 136 * Math.cos(r);
                    const x2 = 150 + 122 * Math.sin(r), y2 = 150 - 122 * Math.cos(r);
                    return <line key={i} x1={x1} y1={y1} x2={x2} y2={y2} />;
                  })}
                </g>
                <text x="150" y="48" textAnchor="middle" fontSize="13" fontWeight="700" fill="#C8F169">K</text>
                <text x="256" y="155" textAnchor="middle" fontSize="13" fontWeight="600" fill="rgba(234,243,236,.6)">D</text>
                <text x="150" y="264" textAnchor="middle" fontSize="13" fontWeight="600" fill="rgba(234,243,236,.6)">G</text>
                <text x="44" y="155" textAnchor="middle" fontSize="13" fontWeight="600" fill="rgba(234,243,236,.6)">B</text>
              </g>
              {b !== null && (
                <g className="needle" style={{ transform: `rotate(${needle}deg)` }}>
                  <path d="M150 62 L 182 176 L 150 160 L 118 176 Z" fill="#C8F169" />
                  <path d="M150 62 L 150 160 L 118 176 Z" fill="#9BCB3F" />
                </g>
              )}
              <circle cx="150" cy="150" r="6" fill="#EAF3EC" />
            </svg>
          </div>
          {d !== null && d < 25 ? <div className="big">{tr.find.near}</div> : <div className="big">{num}<small> {unit}</small></div>}
          <div className="dir">
            {b !== null ? cap(fmt.direction(b)) : tr.find.noPhone}{height ? ` · ${height}` : ""}
          </div>
          {t !== null && <div className="muted">{tr.find.lastFix(fmt.ago(t, m.now))} · {tr.find.quality(p.sats)}</div>}
          {!me && <div className="muted">{tr.find.noPhoneHelp}</div>}
          {me && <div className="muted">{tr.find.accuracy(me.accuracy)}{h === null ? ` · ${tr.find.noCompass}` : ""}</div>}
          {old && (
            <div className="tip"><Clock size={22} color="#C8F169" /><span style={{ fontSize: 14 }}>{tr.find.moved}</span></div>
          )}
          {h !== null && !old && (
            <div className="tip"><Crosshair size={22} color="#C8F169" /><span style={{ fontSize: 14 }}>{tr.find.compassHelp}</span></div>
          )}
        </>
      )}
      <div className="btns">
        <button className="btn" onClick={() => nav.showOnMap(id)}>{tr.common.showOnMap}</button>
        <button className="btn signal" disabled={!p} onClick={() => p && a && openDirections(p.lat, p.lon, a.name)}>{tr.common.directions}</button>
      </div>
    </div>
  );
}
