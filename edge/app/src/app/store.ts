// One hook gives the screens the whole app model: the active data source
// (phone ledger or sample herd), the owner's profiles and preferences, and
// everything derived from them. Screens never touch the sources directly.
import { createContext, useContext, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import type { Point } from "../core/points.ts";
import type { Snapshot } from "./controller.ts";
import { coverageCells, type Coverage } from "./coverage.ts";
import { DEMO_STATION, DEMO_STATION_POS, DemoSource, demoBook } from "./demo.ts";
import { buildDevices } from "./devices.ts";
import { deriveEvents } from "./events.ts";
import type { LatLon } from "./geo.ts";
import { buildHerd } from "./herd.ts";
import type { Animal, AppEvent, AssetProfile, Device, ServerView, Station } from "./model.ts";
import { serverOf, stationOf, type EdgeSource } from "./source.ts";
import { AssetStore, Persisted, prefs, seenEvents, stationPlaces, type Prefs } from "./stores.ts";

export interface Sources {
  real: EdgeSource | null;          // null when not running on the phone (web preview)
  demo: DemoSource;
  realAssets: AssetStore;
  demoAssets: AssetStore;
  demoPlaces: Persisted<Record<string, LatLon>>;
}

export function makeSources(real: EdgeSource | null): Sources {
  const now = Date.now();
  return {
    real, demo: new DemoSource(),
    realAssets: new AssetStore("orun.app.assets.v1"),
    demoAssets: new AssetStore(null, demoBook(now)),
    demoPlaces: new Persisted<Record<string, LatLon>>(null, () => ({ [DEMO_STATION.id]: DEMO_STATION_POS })),
  };
}

export const SourcesContext = createContext<Sources | null>(null);

export interface Model {
  demo: boolean;
  canLeaveDemo: boolean;
  now: number;
  prefs: Prefs;
  snap: Snapshot;
  points: Point[];
  loaded: boolean;
  herd: Animal[];
  devices: Device[];
  station: Station;
  server: ServerView;
  events: AppEvent[];
  coverage: Coverage;
  seen: Set<string>;
  source: EdgeSource;
  actions: Actions;
}

export interface Actions {
  setPrefs(patch: Partial<Prefs>): void;
  saveAsset(p: AssetProfile, device: string | null): void;
  addNote(id: string, device: string | null, text: string): void;
  markStation(pos: LatLon | null): void;
  markSeen(id: string): void;
}

function useNow(ms: number): number {
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const t = setInterval(() => setNow(Date.now()), ms); return () => clearInterval(t); }, [ms]);
  return now;
}

function usePoints(src: EdgeSource, version: number): { points: Point[]; loaded: boolean } {
  const [state, setState] = useState<{ src: EdgeSource; points: Point[]; loaded: boolean }>({ src, points: [], loaded: false });
  const last = useRef(0);
  useEffect(() => {
    let cancelled = false;
    // Bursts of records: at most one ledger read every 2 s.
    const wait = state.src === src ? Math.max(0, 2000 - (Date.now() - last.current)) : 0;
    const t = setTimeout(async () => {
      last.current = Date.now();
      const r = await src.points().catch(() => null);
      if (!cancelled) setState({ src, points: r?.points ?? [], loaded: r !== null });
    }, wait);
    return () => { cancelled = true; clearTimeout(t); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [src, version]);
  return state.src === src ? state : { points: [], loaded: false };
}

export function useModel(): Model {
  const s = useContext(SourcesContext)!;
  const p = useSyncExternalStore(prefs.subscribe, () => prefs.get());
  const demo = s.real === null || p.demo === true;
  const source: EdgeSource = demo ? s.demo : s.real!;
  const assets = demo ? s.demoAssets : s.realAssets;
  const places = demo ? s.demoPlaces : stationPlaces;

  const snap = useSyncExternalStore((fn) => source.subscribe(fn), () => source.snap);
  const book = useSyncExternalStore(assets.subscribe, () => assets.get());
  const placeMap = useSyncExternalStore(places.subscribe, () => places.get());
  const seenList = useSyncExternalStore(seenEvents.subscribe, () => seenEvents.get());
  const { points, loaded } = usePoints(source, snap.pointsVersion);
  const now = useNow(30_000);

  const herd = useMemo(() => buildHerd({ points, profiles: book.assets, assetOf: assets.resolver(), now }),
    [points, book, assets, now]);
  const coverage = useMemo(() => coverageCells(points), [points]);
  // The snapshot changes on every log line; recompute only on what matters.
  const station = useMemo(() => stationOf(snap, placeMap),
    [snap.gateway, snap.gatewayHeld, snap.link, snap.state, snap.detail, placeMap]); // eslint-disable-line react-hooks/exhaustive-deps
  const server = useMemo(() => serverOf(snap), [snap.server]); // eslint-disable-line react-hooks/exhaustive-deps
  const events = useMemo(() => deriveEvents({ herd, station, server, journal: snap.journal, coverage, now }),
    [herd, station, server, snap.journal, coverage, now]);
  const devices = useMemo(() => buildDevices(points, herd, now), [points, herd, now]);
  const seen = useMemo(() => new Set(seenList), [seenList]);

  const actions = useMemo<Actions>(() => ({
    setPrefs: (patch) => prefs.update((v) => ({ ...v, ...patch })),
    saveAsset: (prof, device) => assets.save(prof, device),
    addNote: (id, device, text) => assets.addNote(id, device, text),
    markStation: (pos) => {
      const id = snap.gateway;
      if (!id) return;
      places.update((m) => { const next = { ...m }; if (pos) next[id] = pos; else delete next[id]; return next; });
    },
    markSeen: (id) => seenEvents.update((l) => (l.includes(id) ? l : [...l, id].slice(-500))),
  }), [assets, places, snap.gateway]);

  return {
    demo, canLeaveDemo: s.real !== null, now, prefs: p, snap, points, loaded, herd, devices, station, server,
    events, coverage, seen, source, actions,
  };
}

// Computed once in App, read by every screen.
export const ModelContext = createContext<Model | null>(null);
export function useApp(): Model {
  const m = useContext(ModelContext);
  if (!m) throw new Error("ModelContext missing");
  return m;
}
