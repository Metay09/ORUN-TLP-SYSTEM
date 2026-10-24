import { createRoot } from "react-dom/client";
import { App } from "./ui/App.tsx";
import { Controller } from "./ui/controller.ts";
import { CapacitorDb } from "./platform/sqliteDb.ts";
import "./ui/styles.css";

const controller = new Controller();
void controller.start(() => CapacitorDb.open());
createRoot(document.getElementById("root")!).render(<App controller={controller} />);
