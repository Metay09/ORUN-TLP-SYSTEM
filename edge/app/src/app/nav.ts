// Navigation: five tabs, a stack of pushed screens above them, and a back
// handler stack so Android's back key always closes the top-most thing first
// (dialog -> pushed screen -> map selection -> back to the map tab -> app to
// the background).
import { createContext, useContext, useEffect, useRef } from "react";

export type Tab = "map" | "herd" | "zones" | "events" | "network";
export type Route =
  | { to: "animal"; id: string }
  | { to: "find"; id: string }
  | { to: "device"; id: string }
  | { to: "zone"; id: string }
  | { to: "settings" };

/** What the map shows when another screen sends the user there. */
export interface MapFocus {
  selected: string | null;
  window: { from: number; to: number } | null;   // a single day to replay
  zone: string | null;                             // a zone to frame and highlight
  draw: { zone: string | null } | null;            // start drawing: a new zone (null) or this one
  key: number;                                     // bump to re-apply
}

export interface Nav {
  tab: Tab;
  stack: Route[];
  setTab(t: Tab): void;
  push(r: Route): void;
  pop(): void;
  showOnMap(id: string | null, window?: { from: number; to: number } | null): void;
  showZone(id: string): void;
  drawZone(id: string | null): void;
  focus: MapFocus;
}

export const NavContext = createContext<Nav | null>(null);
export function useNav(): Nav {
  const n = useContext(NavContext);
  if (!n) throw new Error("NavContext missing");
  return n;
}

// ---- back key handlers (top-most first) -----------------------------------
const handlers: { id: number; fn: () => void }[] = [];
let nextId = 1;

/** While mounted (and `active`), the back key calls `fn` before anything else. */
export function useBack(fn: () => void, active = true): void {
  const ref = useRef(fn);
  ref.current = fn;
  useEffect(() => {
    if (!active) return;
    const id = nextId++;
    handlers.push({ id, fn: () => ref.current() });
    return () => { const i = handlers.findIndex((h) => h.id === id); if (i >= 0) handlers.splice(i, 1); };
  }, [active]);
}

/** Runs the top-most handler; false when none is registered. */
export function runBack(): boolean {
  const h = handlers[handlers.length - 1];
  if (!h) return false;
  h.fn();
  return true;
}
