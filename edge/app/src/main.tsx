// Entry: on the phone the real ledger (Controller) starts; in a browser
// (development preview) only the sample herd exists.
import { createRoot } from "react-dom/client";
import { Capacitor } from "@capacitor/core";
import "@fontsource/inter/latin-400.css";
import "@fontsource/inter/latin-ext-400.css";
import "@fontsource/inter/latin-500.css";
import "@fontsource/inter/latin-ext-500.css";
import "@fontsource/inter/latin-600.css";
import "@fontsource/inter/latin-ext-600.css";
import "@fontsource/inter/latin-700.css";
import "@fontsource/inter/latin-ext-700.css";
import "@fontsource/manrope/latin-800.css";
import "@fontsource/manrope/latin-ext-800.css";
import "./ds/ds.css";
import "./features/map/map.css";
import { App } from "./App.tsx";
import { Controller } from "./app/controller.ts";
import { makeSources, SourcesContext } from "./app/store.ts";
import type { EdgeSource } from "./app/source.ts";
import { emptySource } from "./app/emptySource.ts";
import { CapacitorDb } from "./platform/sqliteDb.ts";

let real: EdgeSource | null = null;
if (Capacitor.isNativePlatform()) {
  const c = new Controller();
  void c.start(() => CapacitorDb.open());
  real = c;
} else if (import.meta.env.DEV && location.search.includes("empty")) {
  // Development only: a phone with an empty ledger and no station.
  real = emptySource();
}
createRoot(document.getElementById("root")!).render(
  <SourcesContext.Provider value={makeSources(real)}>
    <App />
  </SourcesContext.Provider>,
);
