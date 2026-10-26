// Bottom sheet over the map with three heights (Material 3 standard sheet):
// peek (summary), half, full. Drag the grip area, or tap it to cycle (the
// single-pointer alternative to dragging).
import { useEffect, useRef, useState, type PointerEvent, type ReactNode } from "react";

export type Snap = "peek" | "half" | "full";

export function snapHeight(snap: Snap, total: number, peek: number, half = 0.46): number {
  if (snap === "peek") return peek;
  if (snap === "half") return Math.max(peek, Math.round(total * half));
  return Math.round(total - 72);
}

interface Props {
  snap: Snap;
  peek: number;
  half?: number;      // share of the screen at "half"
  onSnap: (s: Snap) => void;
  header: ReactNode;
  children: ReactNode;
  onHeight?: (px: number) => void;
}

export function Sheet({ snap, peek, half = 0.46, onSnap, header, children, onHeight }: Props) {
  const box = useRef<HTMLDivElement>(null);
  const [total, setTotal] = useState(() => window.innerHeight);
  const [drag, setDrag] = useState<number | null>(null);
  const start = useRef<{ y: number; h: number; moved: boolean } | null>(null);

  useEffect(() => {
    const parent = box.current?.parentElement;
    if (!parent) return;
    const ro = new ResizeObserver(() => setTotal(parent.clientHeight));
    ro.observe(parent);
    setTotal(parent.clientHeight);
    return () => ro.disconnect();
  }, []);

  const height = drag ?? snapHeight(snap, total, peek, half);
  useEffect(() => { onHeight?.(snapHeight(snap, total, peek, half)); }, [snap, total, peek, half, onHeight]);

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
    if (s.moved) setDrag(Math.min(snapHeight("full", total, peek, half), Math.max(peek - 40, s.h + dy)));
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
    let best: Snap = "peek";
    for (const o of ["peek", "half", "full"] as Snap[])
      if (Math.abs(snapHeight(o, total, peek, half) - h) < Math.abs(snapHeight(best, total, peek, half) - h)) best = o;
    onSnap(best);
    setDrag(null);
  };

  return (
    <div ref={box} className={"sheet" + (drag !== null ? " dragging" : "")} style={{ height }}>
      <div className="sheet-grip" onPointerDown={onDown} onPointerMove={onMove} onPointerUp={onUp}
        onPointerCancel={onUp} role="button" aria-label="Paneli büyüt veya küçült">
        <span className="handle" />
        {header}
      </div>
      <div className="sheet-body">{children}</div>
    </div>
  );
}
