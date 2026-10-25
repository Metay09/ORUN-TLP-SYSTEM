import { createRoot } from "react-dom/client";
import { App } from "./ui/App.tsx";
import { Controller } from "./ui/controller.ts";
import { CapacitorDb } from "./platform/sqliteDb.ts";
import "@fontsource/figtree/latin-400.css";
import "@fontsource/figtree/latin-ext-400.css";
import "@fontsource/figtree/latin-500.css";
import "@fontsource/figtree/latin-ext-500.css";
import "@fontsource/figtree/latin-600.css";
import "@fontsource/figtree/latin-ext-600.css";
import "./ui/styles.css";

const controller = new Controller();
void controller.start(() => CapacitorDb.open());
createRoot(document.getElementById("root")!).render(<App controller={controller} />);
