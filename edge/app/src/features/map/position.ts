// The phone's own position and compass heading. Never mixed up with an
// animal's position: it has its own marker and its own store. Started on
// demand (Konumum, Bul) and kept while the app is open.
import { useSyncExternalStore } from "react";
import type { MyPos } from "./mapEngine.ts";

interface State { pos: MyPos | null; altitude: number | null; altAccuracy: number | null; error: string | null; compass: boolean; }

let state: State = { pos: null, altitude: null, altAccuracy: null, error: null, compass: false };
let watch: number | null = null;
let heading: number | null = null;
let orientationOn = false;
const listeners = new Set<() => void>();
const emit = (patch: Partial<State>) => { state = { ...state, ...patch }; for (const l of listeners) l(); };

function onOrientation(e: DeviceOrientationEvent & { webkitCompassHeading?: number }): void {
  let h: number | null = null;
  if (typeof e.webkitCompassHeading === "number") h = e.webkitCompassHeading;
  else if (e.absolute && e.alpha !== null) {
    const screenAngle = (screen.orientation?.angle ?? 0);
    h = (360 - e.alpha + screenAngle) % 360;
  }
  if (h === null) return;
  // Smooth the needle a little (shortest way round).
  heading = heading === null ? h : (heading + ((((h - heading) % 360) + 540) % 360 - 180) * 0.35 + 360) % 360;
  emit({ compass: true, pos: state.pos ? { ...state.pos, heading } : null });
}

export function startPosition(): void {
  if (!orientationOn && typeof window !== "undefined") {
    orientationOn = true;
    window.addEventListener("deviceorientationabsolute", onOrientation as EventListener);
    window.addEventListener("deviceorientation", onOrientation as EventListener);
  }
  if (watch !== null) return;
  if (typeof navigator === "undefined" || !navigator.geolocation) { emit({ error: "unsupported" }); return; }
  watch = navigator.geolocation.watchPosition(
    (p) => emit({
      pos: { lat: p.coords.latitude, lon: p.coords.longitude, accuracy: p.coords.accuracy, heading },
      altitude: p.coords.altitude, altAccuracy: p.coords.altitudeAccuracy, error: null,
    }),
    (e) => {
      emit({ error: e.code === e.PERMISSION_DENIED ? "denied" : "unavailable" });
      if (e.code === e.PERMISSION_DENIED && watch !== null) { navigator.geolocation.clearWatch(watch); watch = null; }
    },
    { enableHighAccuracy: true, maximumAge: 5000, timeout: 30000 },
  );
}

export function usePosition(): State {
  return useSyncExternalStore((fn) => { listeners.add(fn); return () => listeners.delete(fn); }, () => state);
}
