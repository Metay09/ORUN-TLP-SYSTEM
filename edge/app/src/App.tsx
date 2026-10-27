// ORUN app shell: five tabs (Harita · Sürü · Alanlar · Olaylar · Ağ), pushed
// screens above them, theme / sun mode, and one predictable Android back key.
import { useCallback, useEffect, useMemo, useState, type ReactNode } from "react";
import { App as CapApp } from "@capacitor/app";
import { Bell, CellTower, Cow, MapTrifold, Polygon } from "@phosphor-icons/react";
import { activeCount } from "./app/events.ts";
import { tr } from "./app/i18n/tr.ts";
import { NavContext, runBack, type MapFocus, type Nav, type Route, type Tab } from "./app/nav.ts";
import { ModelContext, useModel } from "./app/store.ts";
import type { Theme } from "./app/stores.ts";
import { EventsScreen } from "./features/events/EventsScreen.tsx";
import { AnimalScreen } from "./features/herd/AnimalScreen.tsx";
import { HerdScreen } from "./features/herd/HerdScreen.tsx";
import { FindScreen } from "./features/map/FindScreen.tsx";
import { MapScreen } from "./features/map/MapScreen.tsx";
import { DeviceScreen } from "./features/network/DeviceScreen.tsx";
import { NetworkScreen } from "./features/network/NetworkScreen.tsx";
import { SettingsScreen } from "./features/settings/SettingsScreen.tsx";
import { ZonesScreen } from "./features/zones/ZonesScreen.tsx";
import { ZoneScreen } from "./features/zones/ZoneScreen.tsx";

function useTheme(theme: Theme, sun: boolean): void {
  useEffect(() => {
    const root = document.documentElement;
    const mq = window.matchMedia?.("(prefers-color-scheme: dark)");
    const apply = () => {
      const dark = theme === "dark" || (theme === "auto" && Boolean(mq?.matches));
      root.dataset.theme = dark ? "dark" : "light";
      document.querySelector('meta[name="theme-color"]')?.setAttribute("content", dark ? "#0F1613" : "#F4F6F2");
    };
    apply();
    if (sun) root.dataset.sun = "1"; else delete root.dataset.sun;
    mq?.addEventListener?.("change", apply);
    return () => mq?.removeEventListener?.("change", apply);
  }, [theme, sun]);
}

const TABS: { tab: Tab; label: string; icon: (on: boolean) => ReactNode }[] = [
  { tab: "map", label: tr.tabs.map, icon: (on) => <MapTrifold size={24} weight={on ? "fill" : "regular"} /> },
  { tab: "herd", label: tr.tabs.herd, icon: (on) => <Cow size={24} weight={on ? "fill" : "regular"} /> },
  { tab: "zones", label: tr.tabs.zones, icon: (on) => <Polygon size={24} weight={on ? "fill" : "regular"} /> },
  { tab: "events", label: tr.tabs.events, icon: (on) => <Bell size={24} weight={on ? "fill" : "regular"} /> },
  { tab: "network", label: tr.tabs.network, icon: (on) => <CellTower size={24} weight={on ? "fill" : "regular"} /> },
];

export function App() {
  const model = useModel();
  useTheme(model.prefs.theme, model.prefs.sun);
  const [tab, setTabState] = useState<Tab>("map");
  const [stack, setStack] = useState<Route[]>([]);
  const [focus, setFocus] = useState<MapFocus>({ selected: null, window: null, zone: null, draw: null, key: 0 });

  const setTab = useCallback((t: Tab) => { setStack([]); setTabState(t); }, []);
  const push = useCallback((r: Route) => setStack((s) => [...s, r]), []);
  const pop = useCallback(() => setStack((s) => s.slice(0, -1)), []);
  const showOnMap = useCallback((id: string | null, window: { from: number; to: number } | null = null) => {
    setStack([]);
    setTabState("map");
    setFocus((f) => ({ selected: id, window, zone: null, draw: null, key: f.key + 1 }));
  }, []);
  const showZone = useCallback((id: string) => {
    setStack([]);
    setTabState("map");
    setFocus((f) => ({ selected: null, window: null, zone: id, draw: null, key: f.key + 1 }));
  }, []);
  const drawZone = useCallback((id: string | null) => {
    setStack([]);
    setTabState("map");
    setFocus((f) => ({ selected: null, window: null, zone: id, draw: { zone: id }, key: f.key + 1 }));
  }, []);
  const nav = useMemo<Nav>(() => ({ tab, stack, setTab, push, pop, showOnMap, showZone, drawZone, focus }),
    [tab, stack, setTab, push, pop, showOnMap, showZone, drawZone, focus]);

  // Android back: the top-most open thing first, then back to the map, then
  // to the background (connections keep running there).
  useEffect(() => {
    const h = CapApp.addListener("backButton", () => {
      if (runBack()) return;
      if (tab !== "map") setTab("map");
      else void CapApp.minimizeApp();
    });
    return () => { void h.then((x) => x.remove()); };
  }, [tab, setTab]);

  const alerts = activeCount(model.events, model.seen);
  const top = stack[stack.length - 1];

  return (
    <ModelContext.Provider value={model}>
      <NavContext.Provider value={nav}>
        <div className="shell">
          <main className="stage">
            <MapScreen visible={tab === "map"} />
            {tab === "herd" && <HerdScreen />}
            {tab === "zones" && <ZonesScreen />}
            {tab === "events" && <EventsScreen />}
            {tab === "network" && <NetworkScreen />}
          </main>
          <nav className="tabbar" role="tablist">
            {TABS.map((t) => (
              <button key={t.tab} className="tab" role="tab" aria-selected={tab === t.tab} onClick={() => setTab(t.tab)}>
                <span className="pip">{t.icon(tab === t.tab)}{t.tab === "events" && alerts > 0 && <span className="dotn" />}</span>
                <span>{t.label}</span>
              </button>
            ))}
          </nav>
          {stack.map((r, i) => (
            <div key={i} hidden={r !== top}>
              {r.to === "animal" && <AnimalScreen id={r.id} />}
              {r.to === "device" && <DeviceScreen id={r.id} />}
              {r.to === "find" && <FindScreen id={r.id} />}
              {r.to === "zone" && <ZoneScreen id={r.id} />}
              {r.to === "settings" && <SettingsScreen />}
            </div>
          ))}
          {model.snap.error && <div className="toast">{tr.states.ledgerError(model.snap.error)}</div>}
        </div>
      </NavContext.Provider>
    </ModelContext.Provider>
  );
}
