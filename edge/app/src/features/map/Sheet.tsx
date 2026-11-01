// ORUN map bottom sheet. Three existing heights and the same visual design.
// Pointer capture is on the entire grip/header, not whichever nested span was
// first touched. All movement/release math reads refs, never stale React state.
import { useEffect, useRef, useState, type PointerEvent, type ReactNode } from "react";
import { clampSheetHeight, settleSheet, snapHeight, type Snap } from "./sheetMotion.ts";
export type { Snap } from "./sheetMotion.ts";
export { snapHeight } from "./sheetMotion.ts";

interface Props {
  snap: Snap;
  peek: number;
  half?: number;      // share of the screen at "half"
  onSnap: (s: Snap) => void;
  header: ReactNode;
  children: ReactNode;
  onHeight?: (px: number) => void;
}

interface DragGesture {
  id: number;
  startY: number;
  startHeight: number;
  lastY: number;
  lastTime: number;
  speed: number; // px/ms; positive upwards
  moved: boolean;
}

export function Sheet({ snap, peek, half = 0.46, onSnap, header, children, onHeight }: Props) {
  const box = useRef<HTMLDivElement>(null);
  const gesture = useRef<DragGesture | null>(null);
  const [total, setTotal] = useState(() => window.innerHeight);
  const [drag, setDrag] = useState<number | null>(null);

  useEffect(() => {
    const parent = box.current?.parentElement;
    if (!parent) return;
    const ro = new ResizeObserver(() => setTotal(parent.clientHeight));
    ro.observe(parent);
    setTotal(parent.clientHeight);
    return () => ro.disconnect();
  }, []);

  const height = drag ?? snapHeight(snap, total, peek, half);
  // Keep map attribution and selected animal padding coordinated with the
  // actual moving panel; the previous implementation reported only snap height.
  useEffect(() => { onHeight?.(height); }, [height, onHeight]);

  const onDown = (e: PointerEvent<HTMLDivElement>) => {
    if (gesture.current || !e.isPrimary || (e.pointerType === "mouse" && e.button !== 0)) return;
    if ((e.target as HTMLElement).closest("button, input, a, select, textarea, [data-sheet-no-drag]")) return;
    const g: DragGesture = {
      id: e.pointerId, startY: e.clientY, startHeight: height,
      lastY: e.clientY, lastTime: e.timeStamp, speed: 0, moved: false,
    };
    gesture.current = g;
    // Capturing on currentTarget fixes loss of the swipe when the pointer
    // crosses children / slides off the tiny bar on a real Android screen.
    try { e.currentTarget.setPointerCapture(e.pointerId); }
    catch { gesture.current = null; }
  };

  const onMove = (e: PointerEvent<HTMLDivElement>) => {
    const g = gesture.current;
    if (!g || g.id !== e.pointerId) return;
    const upward = g.startY - e.clientY;
    if (Math.abs(upward) >= 5) g.moved = true;
    const dt = e.timeStamp - g.lastTime;
    if (dt > 0 && dt < 120) {
      const instant = (g.lastY - e.clientY) / dt;
      g.speed = g.speed * 0.45 + instant * 0.55;
    } else if (dt >= 120) g.speed = 0;
    g.lastY = e.clientY;
    g.lastTime = e.timeStamp;
    if (g.moved) setDrag(clampSheetHeight(g.startHeight + upward, total, peek, half));
  };

  const onUp = (e: PointerEvent<HTMLDivElement>) => {
    const g = gesture.current;
    if (!g || g.id !== e.pointerId) return;
    gesture.current = null;
    const upward = g.startY - e.clientY;
    if (g.moved || Math.abs(upward) >= 5) {
      const releaseHeight = clampSheetHeight(g.startHeight + upward, total, peek, half);
      const speed = e.timeStamp - g.lastTime > 100 ? 0 : g.speed;
      const next = settleSheet(snap, upward, releaseHeight, total, peek, half, speed);
      if (next !== snap) onSnap(next);
    }
    // Taps never change the panel. No new open/close button.
    setDrag(null);
  };

  const cancel = (e: PointerEvent<HTMLDivElement>) => {
    if (gesture.current?.id !== e.pointerId) return;
    gesture.current = null;
    setDrag(null);
  };

  return (
    <div ref={box} className={"sheet" + (drag !== null ? " dragging" : "")} style={{ height }}>
      <div className="sheet-grip" onPointerDown={onDown} onPointerMove={onMove}
        onPointerUp={onUp} onPointerCancel={cancel} onLostPointerCapture={cancel}
        aria-label="Bilgi panelini yukarı veya aşağı sürükle">
        <span className="handle" aria-hidden="true" />
        {header}
      </div>
      <div className="sheet-body">{children}</div>
    </div>
  );
}
