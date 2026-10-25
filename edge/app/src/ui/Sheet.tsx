// Bottom sheet over the map with three heights (Material 3 standard sheet):
// peek (one summary line), half, full. Drag the handle area, or tap it to
// cycle (the single-pointer alternative to dragging).
import { useEffect, useRef, useState, type PointerEvent, type ReactNode } from "react";

export type Snap = "peek" | "half" | "full";

const PEEK = 112;   // px visible when collapsed (handle + summary)

export function snapHeight(snap: Snap, total: number): number {
  if (snap === "peek") return PEEK;
  if (snap === "half") return Math.round(total * 0.5);
  return Math.round(total - 88);
}

interface Props {
  snap: Snap;
  onSnap: (s: Snap) => void;
  header: ReactNode;
  children: ReactNode;
  onHeight?: (px: number) => void;
}

export function Sheet({ snap, onSnap, header, children, onHeight }: Props) {
  const box = useRef<HTMLDivElement>(null);
  const [total, setTotal] = useState(() => window.innerHeight);
  const [drag, setDrag] = useState<number | null>(null);   // live height while dragging
  const start = useRef<{ y: number; h: number; moved: boolean } | null>(null);

  useEffect(() => {
    const parent = box.current?.parentElement;
    if (!parent) return;
    const ro = new ResizeObserver(() => setTotal(parent.clientHeight));
    ro.observe(parent);
    setTotal(parent.clientHeight);
    return () => ro.disconnect();
  }, []);

  const height = drag ?? snapHeight(snap, total);
  useEffect(() => { onHeight?.(snapHeight(snap, total)); }, [snap, total, onHeight]);

  const onDown = (e: PointerEvent) => {
    if ((e.target as HTMLElement).closest("button, input, a, select, textarea")) return;
    start.current = { y: e.clientY, h: height, moved: false };
    (e.target as HTMLElement).setPointerCapture?.(e.pointerId);
  };
  const onMove = (e: PointerEvent) => {
    const s = start.current;
    if (!s) return;
    const dy = s.y - e.clientY;
    if (Math.abs(dy) > 6) s.moved = true;
    if (s.moved) setDrag(Math.min(snapHeight("full", total), Math.max(PEEK - 40, s.h + dy)));
  };
  const onUp = () => {
    const s = start.current;
    start.current = null;
    if (!s) return;
    if (!s.moved) {
      onSnap(snap === "peek" ? "half" : snap === "half" ? "full" : "peek");
      setDrag(null);
      return;
    }
    const h = drag ?? s.h;
    const options: Snap[] = ["peek", "half", "full"];
    let best: Snap = "peek";
    for (const o of options)
      if (Math.abs(snapHeight(o, total) - h) < Math.abs(snapHeight(best, total) - h)) best = o;
    onSnap(best);
    setDrag(null);
  };

  return (
    <div ref={box} className={"sheet" + (drag !== null ? " dragging" : "") + (snap === "peek" && drag === null ? " peek" : "")} style={{ height }}>
      <div className="sheet-grip" onPointerDown={onDown} onPointerMove={onMove} onPointerUp={onUp}
        onPointerCancel={onUp} role="button" aria-label="Paneli büyüt veya küçült">
        <span className="handle" />
        {header}
      </div>
      <div className="sheet-body" aria-hidden={snap === "peek" && drag === null}>{children}</div>
    </div>
  );
}
