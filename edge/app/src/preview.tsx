// LOCAL PREVIEW ONLY (not committed): the app with the test sample herd.
import { createRoot } from "react-dom/client";
import "@fontsource/inter/latin-400.css";
import "@fontsource/inter/latin-ext-400.css";
import "@fontsource/inter/latin-600.css";
import "@fontsource/inter/latin-ext-600.css";
import "@fontsource/manrope/latin-800.css";
import "@fontsource/manrope/latin-ext-800.css";
import "./ds/ds.css";
import "./features/map/map.css";
import { App } from "./App.tsx";
import { makeSources, SourcesContext } from "./app/store.ts";
import { emptySource } from "./app/emptySource.ts";
import { demoPoints, demoBook, DEMO_STATION } from "../test/sampleHerd.ts";

const now = Date.now();
const src = emptySource();
src.points = async () => ({ count: 0, skipped: 0, now: new Date().toISOString(), points: demoPoints(now) });
const q = new URLSearchParams(location.search);
if (q.get("gw")) { src.snap.link = "ble"; src.snap.state = "connected"; src.snap.gateway = DEMO_STATION.id; src.snap.gatewayHeld = "37/2600"; }
const s = makeSources(src);
if (!localStorage.getItem("orun.app.assets.v1")) localStorage.setItem("orun.app.assets.v1", JSON.stringify(demoBook(now)));
createRoot(document.getElementById("root")!).render(<SourcesContext.Provider value={s}><App /></SourcesContext.Provider>);
