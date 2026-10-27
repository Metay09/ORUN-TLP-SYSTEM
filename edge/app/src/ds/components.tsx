// ORUN Saha components. Every screen builds from these so the app speaks one
// visual language (scope doc §14).
import { useEffect, type ReactNode } from "react";
import { CaretLeft, Cow, X } from "@phosphor-icons/react";
import type { Animal, AnimalStatus, Freshness } from "../app/model.ts";
import { tr } from "../app/i18n/tr.ts";

export type Tone = "fresh" | "aging" | "silent" | "alarm" | "info" | "brand";

export function toneOfStatus(s: AnimalStatus): Tone {
  switch (s) {
    case "battery-critical": case "lost": case "out": return "alarm";
    case "silent": case "waiting": return "silent";
    case "aging": case "battery-low": return "aging";
    default: return "fresh";
  }
}

const RING_LEN = 2 * Math.PI * 23;   // r = 23 in a 52 viewBox

function ringColor(tone: Tone): string {
  return { fresh: "var(--fresh)", aging: "var(--aging)", silent: "var(--silent)", alarm: "var(--alarm)", info: "var(--info)", brand: "var(--brand)" }[tone];
}

/** Freshness ring around the animal's colour face (design language). */
export function Ring({ color, ratio, tone, initials, size = 50, dim = false, label }: {
  color: string; ratio: number; tone: Tone; initials: string; size?: number; dim?: boolean; label?: string;
}) {
  const face = Math.round(size * 0.8);
  const dash = Math.max(tone === "silent" ? 10 : 6, ratio * RING_LEN);
  return (
    <span className={"ava" + (dim ? " dim" : "")} style={{ width: size, height: size }} role={label ? "img" : undefined} aria-label={label}>
      <svg viewBox="0 0 52 52" aria-hidden="true">
        <circle className="tr" cx="26" cy="26" r="23" />
        <circle className="ar" cx="26" cy="26" r="23" style={{ stroke: ringColor(tone), strokeDasharray: `${dash} ${RING_LEN}` }} />
      </svg>
      <span className="fc" style={{ width: face, height: face, background: color, fontSize: Math.round(size * 0.32) }}>
        {initials || <Cow size={Math.round(face * 0.58)} weight="bold" />}
      </span>
    </span>
  );
}

export function AnimalRing({ a, size = 50 }: { a: Animal; size?: number }) {
  return <Ring color={a.color} ratio={a.freshRatio} tone={toneOfStatus(a.status)} initials={a.initials} size={size}
    dim={a.freshness === "stale" || a.freshness === "none"} label={`${a.name}: ${a.seen}`} />;
}

export function freshTone(f: Freshness): Tone {
  return f === "fresh" ? "fresh" : f === "aging" ? "aging" : "silent";
}

export function Badge({ tone, children }: { tone: Tone; children: ReactNode }) {
  return <span className={`badge b-${tone}`}>{children}</span>;
}

export function Cell({ lead, title, sub, subTone, trail, onClick, children, wrapSub }: {
  lead?: ReactNode; title: ReactNode; sub?: ReactNode; subTone?: Tone; trail?: ReactNode;
  onClick?: () => void; children?: ReactNode; wrapSub?: boolean;
}) {
  const body = (
    <>
      {lead}
      <span className="cm">
        <span className="ct">{title}</span>
        {sub !== undefined && sub !== null && sub !== "" &&
          <span className={"cs" + (subTone ? ` t-${subTone} strong` : "") + (wrapSub ? " wrap" : "")}>{sub}</span>}
        {children}
      </span>
      {trail}
    </>
  );
  return onClick ? <button className="cell" onClick={onClick}>{body}</button> : <div className="cell">{body}</div>;
}

export function Card({ children, pad, hero, className }: { children: ReactNode; pad?: boolean; hero?: boolean; className?: string }) {
  return <div className={"card" + (pad ? " pad16" : "") + (hero ? " hero" : "") + (className ? ` ${className}` : "")}>{children}</div>;
}

export function Section({ title, action, onAction }: { title: string; action?: string; onAction?: () => void }) {
  return (
    <div className="sec">
      <h2>{title}</h2>
      {action && onAction && <button className="lnk" onClick={onAction}>{action}</button>}
    </div>
  );
}

export function KV({ k, children }: { k: string; children: ReactNode }) {
  return <div className="kv"><span>{k}</span><b>{children}</b></div>;
}

export function Seg<T extends string>({ value, options, onChange }: {
  value: T; options: { value: T; label: string }[]; onChange: (v: T) => void;
}) {
  return (
    <div className="seg" role="group">
      {options.map((o) => (
        <button key={o.value} aria-pressed={o.value === value} onClick={() => onChange(o.value)}>{o.label}</button>
      ))}
    </div>
  );
}

export function Chips<T extends string>({ value, options, onChange }: {
  value: T; options: { value: T; label: string; n?: number }[]; onChange: (v: T) => void;
}) {
  return (
    <div className="chips" role="group">
      {options.map((o) => (
        <button key={o.value} className="chip" aria-pressed={o.value === value} onClick={() => onChange(o.value)}>
          {o.label}{o.n !== undefined && o.n > 0 && <span className="n">{o.n}</span>}
        </button>
      ))}
    </div>
  );
}

export function Toggle({ on, onChange, label }: { on: boolean; onChange: (v: boolean) => void; label: string }) {
  return (
    <button className="tog" role="switch" aria-checked={on} aria-label={label} onClick={() => onChange(!on)}><i /></button>
  );
}

export function ToggleRow({ title, sub, on, onChange }: { title: string; sub?: string; on: boolean; onChange: (v: boolean) => void }) {
  return <Cell title={title} sub={sub} wrapSub trail={<Toggle on={on} onChange={onChange} label={title} />} onClick={() => onChange(!on)} />;
}

export function Metric({ label, value, sub }: { label: string; value: ReactNode; sub?: ReactNode }) {
  return (
    <div className="metric">
      <span className="cap">{label}</span>
      <b>{value}</b>
      {sub && <span className="cap ellip">{sub}</span>}
    </div>
  );
}

export function Empty({ icon, title, body, children }: { icon: ReactNode; title: string; body?: string; children?: ReactNode }) {
  return (
    <div className="empty">
      <span className="ic brand">{icon}</span>
      <h3>{title}</h3>
      {body && <p>{body}</p>}
      {children}
    </div>
  );
}

export function Header({ title, onBack, children, center }: { title?: string; onBack?: () => void; children?: ReactNode; center?: boolean }) {
  return (
    <div className="hdr">
      {onBack && <button className="circ" onClick={onBack} aria-label={tr.common.back}><CaretLeft size={22} weight="bold" /></button>}
      {title !== undefined && (center ? <span className="ttl ellip">{title}</span> : <h1 className="ellip">{title}</h1>)}
      {!title && <span className="grow" />}
      {children}
    </div>
  );
}

/** Full-screen pushed page (profile, device, settings…). */
export function Screen({ title, onBack, actions, children, center }: {
  title?: string; onBack: () => void; actions?: ReactNode; children: ReactNode; center?: boolean;
}) {
  return (
    <div className="screen" role="dialog" aria-label={title}>
      <Header title={title} onBack={onBack} center={center}>{actions}</Header>
      <div className="page">{children}</div>
    </div>
  );
}

/** Bottom dialog for short edits. */
export function Dialog({ title, onClose, children }: { title: string; onClose: () => void; children: ReactNode }) {
  useEffect(() => {
    const k = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, [onClose]);
  return (
    <div className="scrim" onClick={onClose}>
      <div className="dialog" role="dialog" aria-label={title} onClick={(e) => e.stopPropagation()}>
        <div className="hdr"><h1>{title}</h1><button className="circ" onClick={onClose} aria-label={tr.common.close}><X size={20} /></button></div>
        <div className="dialog-body">{children}</div>
      </div>
    </div>
  );
}

/** 24 one-hour bars: darker = more movement that hour; empty = no record. */
export function Strip({ values, tall }: { values: (number | null)[]; tall?: boolean }) {
  return (
    <span className={"strip" + (tall ? " tall" : "")} aria-hidden="true">
      {values.map((v, i) => (
        <i key={i} style={v === null ? undefined : { background: `color-mix(in srgb, var(--fresh) ${Math.round(18 + v * 82)}%, var(--sunk))` }} />
      ))}
    </span>
  );
}

/** Small line chart (battery). */
export function Sparkline({ values, min, max, height = 64, tone = "fresh" }: {
  values: { t: number; v: number }[]; min: number; max: number; height?: number; tone?: Tone;
}) {
  if (values.length < 2) return null;
  const t0 = values[0].t, t1 = values[values.length - 1].t || t0 + 1;
  const W = 300, H = height;
  const pts = values.map((p) => `${(((p.t - t0) / (t1 - t0 || 1)) * W).toFixed(1)},${(H - 4 - ((p.v - min) / (max - min || 1)) * (H - 8)).toFixed(1)}`);
  return (
    <svg viewBox={`0 0 ${W} ${H}`} width="100%" height={H} preserveAspectRatio="none" aria-hidden="true">
      <polyline points={pts.join(" ")} fill="none" stroke={ringColor(tone)} strokeWidth="2" vectorEffect="non-scaling-stroke" strokeLinejoin="round" />
    </svg>
  );
}

export function Bar({ ratio, tone }: { ratio: number; tone?: "warn" | "bad" }) {
  return <div className="bar"><i className={tone ?? ""} style={{ width: `${Math.round(Math.max(0, Math.min(1, ratio)) * 100)}%` }} /></div>;
}

export function DemoFlag({ onPage }: { onPage?: boolean }) {
  return <span className={"demo-flag" + (onPage ? " on-page" : "")}>{tr.app.demoBadge}</span>;
}
