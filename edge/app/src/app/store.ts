// One hook gives the screens the whole app model: the active data source
// (the phone ledger), the owner's profiles and preferences, and
// everything derived from them. Screens never touch the sources directly.
import { createContext, useContext, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import type { Point, RecordId } from "../core/points.ts";
import type { Snapshot } from "./controller.ts";
import { deviceNames, deviceSettings, onPilotAccountChange, onPilotObservationSync, pilotAccountSettled, pilotSetHerdZones, pilotAccountStatus, pilotObservationSyncState } from "./ownerAccountPilotRuntime.ts";
import { nextAlerts, watchFresh } from "./alerts.ts";
import { postAlerts } from "../platform/notify.ts";
import { coverageCells, type Coverage } from "./coverage.ts";
import { buildDevices } from "./devices.ts";
import { deriveEvents } from "./events.ts";
import type { LatLon } from "./geo.ts";
import { buildHerd } from "./herd.ts";
import type { Animal, AppEvent, AssetProfile, Device, ServerView, Station } from "./model.ts";
import { serverOf, stationOf, type EdgeSource } from "./source.ts";
import { AssetStore, ZoneStore, alertMemory, prefs, seenEvents, stationPlaces, type Prefs } from "./stores.ts";
import { NO_GRAZE, checkSet, compileFence, grazing, type Fence, type Graze, type SetCheck, type Zone } from "./zones.ts";
import { ZoneTargetStore, type TargetBook, type ZoneGroup, type ZoneRule } from "./zoneTargets.ts";

export interface Sources {
  real: EdgeSource;                 // the phone ledger (an empty one in the web preview)
  realAssets: AssetStore;
  realZones: ZoneStore;
  realTargets: ZoneTargetStore;
}

export function makeSources(real: EdgeSource): Sources {
  return {
    real,
    realAssets: new AssetStore("orun.app.assets.v1"),
    realZones: new ZoneStore("orun.app.zones.v1"),
    realTargets: new ZoneTargetStore("orun.app.zoneTargets.v1"),
  };
}

export const SourcesContext = createContext<Sources | null>(null);

export interface Model {
  now: number;
  prefs: Prefs;
  snap: Snapshot;
  points: Point[];
  /** Records without a position (only for counting sequence gaps). */
  unlocated: RecordId[];
  loaded: boolean;
  /** Just opened: the phone ledger or the sign-in is not read yet. An empty herd is not "no animals". */
  starting: boolean;
  herd: Animal[];
  devices: Device[];
  station: Station;
  server: ServerView;
  events: AppEvent[];
  coverage: Coverage;
  zones: Zone[];
  zoneSet: SetCheck;      // the active set against the collar's limits
  zoneMigrationWarning: string | null; // pre-merge legacy drafts must not disappear silently
  targetBook: TargetBook; // local-only intended device/group targets; never an applied state
  targetWarning: string | null;
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
  unmarkSeen(id: string): void;
  saveZone(z: Zone): void;
  removeZone(id: string): void;
  addZones(list: Zone[]): void;
  saveTargetRule(zoneId: string, rule: ZoneRule): void;
  saveTargetGroup(group: ZoneGroup): void;
  removeTargetGroup(id: string): void;
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
function usePoints(src: EdgeSource, version: number, accountKey: string): { points: Point[]; unlocated: RecordId[]; loaded: boolean; settled: boolean } {
  const [state, setState] = useState<{ src: EdgeSource; accountKey: string; points: Point[]; unlocated: RecordId[]; loaded: boolean; version: number }>({
    src, accountKey, points: [], unlocated: [], loaded: false, version: -1,
  });
  const invalidate = useRef<() => void>(() => {});
  // The ledger version a read started from: `settled` says the points on
  // screen include everything the ledger held when it last changed.
  const wanted = useRef(version);
  wanted.current = version;

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
        const from = wanted.current;
        const result = await src.points().catch(() => null);
        if (active) setState({
          src, accountKey, points: result?.points ?? [], unlocated: result?.unlocated ?? [], loaded: result !== null,
          version: result !== null ? from : -1,
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
  }, [src, accountKey]);

  // Invalidation is deliberately separate from the effect which owns the
  // timer: new Gateway packets must not clear/reschedule an earlier read.
  useEffect(() => { invalidate.current(); }, [src, version, accountKey]);
  // A logout, account switch or expiry must hide cached remote coordinates
  // synchronously, not one effect tick (or a two-second throttle) later.
  return state.src === src && state.accountKey === accountKey
    ? { ...state, settled: state.version === version } : { points: [], unlocated: [], loaded: false, settled: false };
}

export function useModel(): Model {
  const s = useContext(SourcesContext)!;
  const p = useSyncExternalStore(prefs.subscribe, () => prefs.get());
  const source: EdgeSource = s.real;
  const assets = s.realAssets;
  const places = stationPlaces;
  const zoneStore = s.realZones;
  const targets = s.realTargets;

  const snap = useSyncExternalStore((fn) => source.subscribe(fn), () => source.snap);
  const book = useSyncExternalStore(assets.subscribe, () => assets.get());
  const placeMap = useSyncExternalStore(places.subscribe, () => places.get());
  const seenList = useSyncExternalStore(seenEvents.subscribe, () => seenEvents.get());
  const zoneBook = useSyncExternalStore(zoneStore.subscribe, () => zoneStore.get());
  const targetBook = useSyncExternalStore(targets.subscribe, targets.get);
  const account = pilotAccountStatus();
  const accountKey = account
    ? [account.site, account.actor, account.installation, account.username, account.expiresAt].join(":")
    : "";
  const { points, unlocated, loaded, settled } = usePoints(source, snap.pointsVersion, accountKey);
  const now = useNow(30_000);
  const accountSettled = useSyncExternalStore(onPilotAccountChange, pilotAccountSettled);
  const starting = !snap.error && (!snap.ready || !accountSettled || !loaded);

  const zones = zoneBook.zones;
  const graze = zoneBook.graze ?? NO_GRAZE;
  const fence = useMemo(() => compileFence(zones, zoneBook.changed), [zones, zoneBook.changed]);
  const zoneSet = useMemo(() => checkSet(zones), [zones]);
  const grazingNow = grazing(graze, now) || (graze.until !== null && graze.until > Date.now());
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const settings = useSyncExternalStore((fn) => deviceSettings.subscribe(fn), () => deviceSettings.snapshot());
  const intervals = useMemo(() => {
    const out: Record<string, { s: number; at: number }> = {};
    for (const [id, e] of Object.entries(settings)) {
      const v = e.reported?.interval_s;
      if (typeof v === "number" && v > 0 && e.reportedAt) out[id.toUpperCase()] = { s: v, at: e.reportedAt };
    }
    return out;
  }, [settings]);
  // Until the first sync of this run answers (or 15 s pass, or there is no
  // account), a record newer than the ones on the phone may be on its way.
  const syncState = useSyncExternalStore(onPilotObservationSync, pilotObservationSyncState);
  const [openedAt] = useState(() => Date.now());
  const [settleTimeout, setSettleTimeout] = useState(false);
  useEffect(() => { const t = setTimeout(() => setSettleTimeout(true), 15_000); return () => clearTimeout(t); }, []);
  const settling = !settleTimeout && account !== null &&
    !((syncState.last?.at ?? 0) >= openedAt) && syncState.error === null;
  const herd = useMemo(() => buildHerd({ points, profiles: book.assets, assetOf: assets.resolver(), now, fence, grazing: grazingNow, names, unlocated, intervals, settling }),
    [points, book, assets, now, fence, grazingNow, names, unlocated, intervals, settling]);
  const coverage = useMemo(() => coverageCells(points), [points]);
  // The snapshot changes on every log line; recompute only on what matters.
  const station = useMemo(() => stationOf(snap, placeMap),
    [snap.gateway, snap.gatewayHeld, snap.link, snap.state, snap.detail, placeMap]); // eslint-disable-line react-hooks/exhaustive-deps
  const server = useMemo(() => serverOf(snap), [snap.server]); // eslint-disable-line react-hooks/exhaustive-deps
  const events = useMemo(() => deriveEvents({ herd, station, server, journal: snap.journal, coverage, now, fence, grazing: grazingNow }),
    [herd, station, server, snap.journal, coverage, now, fence, grazingNow]);
  // Phone notifications (alerts.ts): decided only on current data, each once.
  const lastSync = useSyncExternalStore(onPilotObservationSync, () => pilotObservationSyncState().last);
  const linked = snap.link !== "none" && snap.state === "connected";
  // The server decides the alerts for a signed-in phone and needs the zones.
  useEffect(() => { pilotSetHerdZones(zoneBook); }, [zoneBook]);
  const posting = useRef(false);
  useEffect(() => {
    // `settled`: the sync that just finished wrote new records, but the screen
    // still holds the old ones for a moment. Deciding then told the owner
    // "Hayvan 2 sessiz" on opening the app (his phone, 2026-11-14 01:06).
    // Signed in and the server reachable: the native herd watch notifies
    // (the server decides; works with the app closed). This copy only covers
    // a phone attached to a Gateway that cannot reach the server.
    if (account && watchFresh(lastSync, false, Date.now())) return;
    if (posting.current || !loaded || !settled || !linked) return;
    const before = alertMemory.get();
    const r = nextAlerts(events, before, Date.now());
    if (r.memory === before) return;
    if (!r.alerts.length) { alertMemory.set(r.memory); return; }
    // Remembered only once Android took them; a refused permission retries later.
    posting.current = true;
    void postAlerts(r.alerts).then((ok) => { if (ok) alertMemory.set(r.memory); }, () => {})
      .finally(() => { posting.current = false; });
  }, [events, loaded, settled, lastSync, linked]);
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
    unmarkSeen: (id) => seenEvents.update((l) => l.filter((x) => x !== id)),
    saveZone: (z) => zoneStore.save(z),
    removeZone: (id) => zoneStore.remove(id),
    addZones: (list) => zoneStore.add(list),
    saveTargetRule: (id, rule) => targets.saveRule(id, rule),
    saveTargetGroup: (group) => targets.saveGroup(group),
    removeTargetGroup: (id) => targets.removeGroup(id),
    setGraze: (h) => zoneStore.setGraze(h === null ? { ...NO_GRAZE } : h === "forever" ? { until: null, forever: true }
      : { until: Date.now() + h * 3600_000, forever: false }),
  }), [assets, places, snap.gateway, zoneStore, targets]);

  return {
    now, prefs: p, snap, points, unlocated, loaded, starting, herd, devices, station, server,
    events, coverage, zones, zoneSet, zoneMigrationWarning: zoneStore.migrationWarning,
    targetBook, targetWarning: targets.warning,
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
