// One hook gives the screens the whole app model: the active data source
// (phone ledger or sample herd), the owner's profiles and preferences, and
// everything derived from them. Screens never touch the sources directly.
import { createContext, useContext, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import type { Point } from "../core/points.ts";
import type { Snapshot } from "./controller.ts";
import { coverageCells, type Coverage } from "./coverage.ts";
import { DEMO_STATION, DEMO_STATION_POS, DemoSource, demoBook, demoZones } from "./demo.ts";
import { buildDevices } from "./devices.ts";
import { deriveEvents } from "./events.ts";
import type { LatLon } from "./geo.ts";
import { buildHerd } from "./herd.ts";
import type { Animal, AppEvent, AssetProfile, Device, ServerView, Station } from "./model.ts";
import { serverOf, stationOf, type EdgeSource } from "./source.ts";
import { AssetStore, Persisted, ZoneStore, prefs, seenEvents, stationPlaces, type Prefs } from "./stores.ts";
import { NO_GRAZE, checkSet, compileFence, grazing, type Fence, type Graze, type SetCheck, type Zone } from "./zones.ts";

export interface Sources {
  real: EdgeSource | null;          // null when not running on the phone (web preview)
  demo: DemoSource;
  realAssets: AssetStore;
  demoAssets: AssetStore;
  demoPlaces: Persisted<Record<string, LatLon>>;
  realZones: ZoneStore;
  demoZones: ZoneStore;
}

export function makeSources(real: EdgeSource | null): Sources {
  const now = Date.now();
  return {
    real, demo: new DemoSource(),
    realAssets: new AssetStore("orun.app.assets.v1"),
    demoAssets: new AssetStore(null, demoBook(now)),
    demoPlaces: new Persisted<Record<string, LatLon>>(null, () => ({ [DEMO_STATION.id]: DEMO_STATION_POS })),
    realZones: new ZoneStore("orun.app.zones.v1"),
    demoZones: new ZoneStore(null, demoZones()),
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
  zones: Zone[];
  zoneSet: SetCheck;      // the active set against the collar's limits
  zoneMigrationWarning: string | null; // pre-merge legacy drafts must not disappear silently
  fence: Fence;           // compiled active, valid zones
  graze: Graze;
  grazing: boolean;       // serbest otlatma on right now
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
  saveZone(z: Zone): void;
  removeZone(id: string): void;
  addZones(list: Zone[]): void;
  /** hours > 0: timed; "forever": until turned off; null: off. */
  setGraze(hours: number | "forever" | null): void;
}

function useNow(ms: number): number {
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const t = setInterval(() => setNow(Date.now()), ms); return () => clearInterval(t); }, [ms]);
  return now;
}

/** Reads the durable phone ledger at most once per 2 s, WITHOUT debounce
 * starvation during a continuous Gateway dump. A newly received head record
 * must be able to reach the map while older records are still arriving.
 *
 * The previous [src, version] effect cancelled its pending timeout for every
 * 250-ms burst and could therefore suppress *all* map updates until DUMP END.
 */
function usePoints(src: EdgeSource, version: number): { points: Point[]; loaded: boolean } {
  const [state, setState] = useState<{ src: EdgeSource; points: Point[]; loaded: boolean }>({
    src, points: [], loaded: false,
  });
  const invalidate = useRef<() => void>(() => {});

  useEffect(() => {
    let active = true;
    let pending: ReturnType<typeof setTimeout> | null = null;
    let reading = false;
    let dirty = false;
    let lastStarted = 0;

    const schedule = () => {
      if (!active || pending !== null || reading || !dirty) return;
      // Leading + trailing throttle. Bursts cannot indefinitely reset this
      // deadline; a second read follows when the ledger changed mid-read.
      const wait = Math.max(0, 2000 - (Date.now() - lastStarted));
      pending = setTimeout(async () => {
        pending = null;
        if (!active || !dirty) return;
        dirty = false;
        reading = true;
        lastStarted = Date.now();
        const result = await src.points().catch(() => null);
        if (active) setState({
          src, points: result?.points ?? [], loaded: result !== null,
        });
        reading = false;
        schedule();
      }, wait);
    };
    invalidate.current = () => { dirty = true; schedule(); };
    invalidate.current();

    return () => {
      active = false;
      if (pending !== null) clearTimeout(pending);
      invalidate.current = () => {};
    };
  }, [src]);

  // Invalidation is deliberately separate from the effect which owns the
  // timer: new Gateway packets must not clear/reschedule an earlier read.
  useEffect(() => { invalidate.current(); }, [src, version]);
  return state.src === src ? state : { points: [], loaded: false };
}

export function useModel(): Model {
  const s = useContext(SourcesContext)!;
  const p = useSyncExternalStore(prefs.subscribe, () => prefs.get());
  const demo = s.real === null || p.demo === true;
  const source: EdgeSource = demo ? s.demo : s.real!;
  const assets = demo ? s.demoAssets : s.realAssets;
  const places = demo ? s.demoPlaces : stationPlaces;
  const zoneStore = demo ? s.demoZones : s.realZones;

  const snap = useSyncExternalStore((fn) => source.subscribe(fn), () => source.snap);
  const book = useSyncExternalStore(assets.subscribe, () => assets.get());
  const placeMap = useSyncExternalStore(places.subscribe, () => places.get());
  const seenList = useSyncExternalStore(seenEvents.subscribe, () => seenEvents.get());
  const zoneBook = useSyncExternalStore(zoneStore.subscribe, () => zoneStore.get());
  const { points, loaded } = usePoints(source, snap.pointsVersion);
  const now = useNow(30_000);

  const zones = zoneBook.zones;
  const graze = zoneBook.graze ?? NO_GRAZE;
  const fence = useMemo(() => compileFence(zones), [zones]);
  const zoneSet = useMemo(() => checkSet(zones), [zones]);
  const grazingNow = grazing(graze, now) || (graze.until !== null && graze.until > Date.now());
  const herd = useMemo(() => buildHerd({ points, profiles: book.assets, assetOf: assets.resolver(), now, fence, grazing: grazingNow }),
    [points, book, assets, now, fence, grazingNow]);
  const coverage = useMemo(() => coverageCells(points), [points]);
  // The snapshot changes on every log line; recompute only on what matters.
  const station = useMemo(() => stationOf(snap, placeMap),
    [snap.gateway, snap.gatewayHeld, snap.link, snap.state, snap.detail, placeMap]); // eslint-disable-line react-hooks/exhaustive-deps
  const server = useMemo(() => serverOf(snap), [snap.server]); // eslint-disable-line react-hooks/exhaustive-deps
  const events = useMemo(() => deriveEvents({ herd, station, server, journal: snap.journal, coverage, now, fence, grazing: grazingNow }),
    [herd, station, server, snap.journal, coverage, now, fence, grazingNow]);
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
    saveZone: (z) => zoneStore.save(z),
    removeZone: (id) => zoneStore.remove(id),
    addZones: (list) => zoneStore.add(list),
    setGraze: (h) => zoneStore.setGraze(h === null ? { ...NO_GRAZE } : h === "forever" ? { until: null, forever: true }
      : { until: Date.now() + h * 3600_000, forever: false }),
  }), [assets, places, snap.gateway, zoneStore]);

  return {
    demo, canLeaveDemo: s.real !== null, now, prefs: p, snap, points, loaded, herd, devices, station, server,
    events, coverage, zones, zoneSet, zoneMigrationWarning: zoneStore.migrationWarning,
    fence, graze, grazing: grazingNow, seen, source, actions,
  };
}

// Computed once in App, read by every screen.
export const ModelContext = createContext<Model | null>(null);
export function useApp(): Model {
  const m = useContext(ModelContext);
  if (!m) throw new Error("ModelContext missing");
  return m;
}
