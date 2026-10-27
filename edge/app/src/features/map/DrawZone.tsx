// Alan çizimi over the map (the field prototype's editor): a cross-hair that
// stays at the screen centre ("Nişangâh": pan the map, add the corner under
// the cross) or plain taps ("Dokunarak"); drag a corner to move it, tap an
// edge's middle dot to add one there; the phone's own position as a corner
// for walking the fence. Live area, corner budget and the collar's own
// geometry rules, with "Sadeleştir" when the corners do not fit.
import { useEffect, useMemo, useRef, useState } from "react";
import { ArrowCounterClockwise, Crosshair, HandTap, MapPin, Plus } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useBack } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import {
  ERROR_TEXT, FENCE, areaM2, canonicalE7, perimeterM, roomFor, simplifyRing, validateRing, newZoneId, type LonLat, type Zone,
} from "../../app/zones.ts";
import { Dialog } from "../../ds/components.tsx";
import type { MapEngine } from "./mapEngine.ts";
import { startPosition, usePosition } from "./position.ts";

export interface DrawHooks {
  onDraft(ring: LonLat[]): void;
  onTap(p: LonLat): void;
  onCentre(p: LonLat): void;
}

export function DrawZone({ engine, zone, hooks, onDone }: {
  engine: MapEngine | null;
  zone: Zone | null;                       // null: a new zone
  hooks: { current: DrawHooks | null };
  onDone: (saved: Zone | null) => void;
}) {
  const m = useApp();
  const pos = usePosition();
  const [ring, setRingState] = useState<LonLat[]>(() => zone ? zone.ring.map((p) => [p[0], p[1]] as LonLat) : []);
  const [cross, setCross] = useState(zone === null);
  const [centre, setCentre] = useState<LonLat | null>(null);
  const [naming, setNaming] = useState(false);
  const [name, setName] = useState(zone?.name ?? tr.zones.defaultName(m.zones.length + 1));
  const [note, setNote] = useState<string | null>(null);
  const history = useRef<LonLat[][]>([]);
  const changed = useRef(false);

  const setRing = (next: LonLat[]) => {
    history.current.push(ring);
    if (history.current.length > 100) history.current.shift();
    changed.current = true;
    setRingState(next);
  };

  // The engine reports drags, taps and the moving centre through these.
  hooks.current = {
    onDraft: (r) => setRing(r),
    onTap: (p) => setRing([...ring, p]),
    onCentre: (p) => setCentre(p),
  };
  useEffect(() => () => { hooks.current = null; }, [hooks]);
  useEffect(() => { startPosition(); }, []);
  useEffect(() => { engine?.setDraft({ ring, cross }); }, [engine, ring, cross]);
  useEffect(() => () => engine?.setDraft(null), [engine]);
  useEffect(() => { if (zone && engine) engine.fitRing(zone.ring); }, [zone, engine]);
  useEffect(() => { if (!note) return; const t = setTimeout(() => setNote(null), 6000); return () => clearTimeout(t); }, [note]);

  const willBeActive = zone ? zone.active : true;
  const room = roomFor(m.zones, zone?.id ?? null, willBeActive);
  const measured = cross && centre && ring.length >= 2 ? [...ring, centre] : ring;
  const a = measured.length >= 3 ? areaM2(measured) : 0;
  const n = canonicalE7(ring).length;
  const over = n > room;
  const err = ring.length >= 3 ? validateRing(ring) : null;
  const perim = useMemo(() => perimeterM(measured, measured.length >= 3), [measured]);

  const cancel = () => {
    if (changed.current && !window.confirm(tr.zones.draw.leave)) return;
    onDone(null);
  };
  useBack(cancel, !naming);
  useBack(() => setNaming(false), naming);

  const simplify = () => {
    const s = simplifyRing(ring, Math.max(3, room));
    if (!s) { setNote(tr.zones.draw.simplifyFailed); return; }
    const a0 = areaM2(ring);
    setNote(tr.zones.draw.simplified(ring.length, s.length, fmt.dec(a0 ? (Math.abs(areaM2(s) - a0) / a0) * 100 : 0)));
    setRing(s);
  };

  const save = () => {
    const z: Zone = {
      id: zone?.id ?? newZoneId(), name: name.trim() || tr.zones.defaultName(m.zones.length + 1), ring,
      active: zone ? zone.active : m.zoneSet.active.length < FENCE.maxAreas, updated: Date.now(),
    };
    m.actions.saveZone(z);
    onDone(z);
  };

  const hint = cross
    ? (ring.length ? tr.zones.draw.hintCross(ring.length) : tr.zones.draw.hintCross0)
    : (ring.length ? tr.zones.draw.hintTap(ring.length) : tr.zones.draw.hintTap0);
  const canSave = ring.length >= 3 && !err && !over;

  return (
    <>
      <header className="draw-top">
        <div className="row" style={{ justifyContent: "space-between" }}>
          <button className="btn quiet" onClick={cancel}>{tr.zones.draw.cancel}</button>
          <span className="ttl">{zone ? tr.zones.draw.editTitle : tr.zones.draw.newTitle}</span>
          <button className="btn primary" disabled={!canSave} onClick={() => setNaming(true)}>{tr.zones.draw.save}</button>
        </div>
        <div className="draw-stats">
          <b>{measured.length >= 3 ? tr.zones.draw.stats(fmt.dec(a / 10_000, 2), fmt.dec(a / 1000, 1)) : "— ha"}</b>
          <span className={over ? "t-alarm strong" : ""}>{tr.zones.draw.corners(n, room)}</span>
          {measured.length >= 2 && <span>{tr.zones.draw.perimeter(fmt.distance(perim))}</span>}
        </div>
      </header>

      {cross && (
        <div className="draw-cross" aria-hidden="true"><i /><i /><b /></div>
      )}
      <section className="draw-bar" aria-label="Çizim araçları">
        <span className="draw-hint" aria-live="polite">{note ?? hint}</span>
        {(over || err) && (
          <div className="draw-fix">
            <span className="t-alarm strong">{over ? tr.zones.draw.over : tr.zones.refused(ERROR_TEXT[err!])}</span>
            {over && <button className="btn small primary" onClick={simplify}>{tr.zones.draw.simplify}</button>}
          </div>
        )}
        <div className="row">
          <button className="btn sq" onClick={() => { const prev = history.current.pop(); if (prev) setRingState(prev); }}
            disabled={!history.current.length} aria-label={tr.zones.draw.undo}><ArrowCounterClockwise size={22} /></button>
          {cross ? (
            <button className="btn signal grow" onClick={() => engine && setRing([...ring, engine.centre()])}>
              <Plus size={20} weight="bold" />{tr.zones.draw.add}
            </button>
          ) : (
            <button className="btn grow" onClick={() => pos.pos && setRing([...ring, [pos.pos.lon, pos.pos.lat]])} disabled={!pos.pos}>
              <MapPin size={20} />{pos.pos ? tr.zones.draw.mineAcc(pos.pos.accuracy) : tr.zones.draw.noFix}
            </button>
          )}
          <button className="btn tall" onClick={() => setCross((c) => !c)} aria-pressed={!cross}>
            {cross ? <HandTap size={20} /> : <Crosshair size={20} />}<span>{cross ? tr.zones.draw.tap : tr.zones.draw.cross}</span>
          </button>
        </div>
        {cross && pos.pos && (
          <button className="btn quiet wide" onClick={() => setRing([...ring, [pos.pos!.lon, pos.pos!.lat]])}>
            <MapPin size={18} />{tr.zones.draw.mineAcc(pos.pos.accuracy)}
          </button>
        )}
      </section>

      {naming && (
        <Dialog title={tr.zones.name} onClose={() => setNaming(false)}>
          <div className="stack" style={{ padding: "4px 16px 16px" }}>
            <span className="sm">{tr.zones.draw.stats(fmt.dec(areaM2(ring) / 10_000, 2), fmt.dec(areaM2(ring) / 1000, 1))} · {tr.zones.draw.perimeter(fmt.distance(perimeterM(ring)))} · {n} köşe</span>
            <input className="inp" value={name} maxLength={40} autoFocus onChange={(e) => setName(e.target.value)}
              aria-label={tr.zones.name} onKeyDown={(e) => { if (e.key === "Enter") save(); }} />
            <button className="btn primary wide" onClick={save}>{tr.zones.draw.save}</button>
          </div>
        </Dialog>
      )}
    </>
  );
}
