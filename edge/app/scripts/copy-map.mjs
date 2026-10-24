// The phone app shows the same map page as the PC tool (one source of truth:
// tools/edge_map). Copied into public/ before every dev/build run.
import { cpSync, mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";

const src = fileURLToPath(new URL("../../../tools/edge_map/", import.meta.url));
const pub = fileURLToPath(new URL("../public/", import.meta.url));
mkdirSync(pub, { recursive: true });
cpSync(src + "index.html", pub + "map.html");
cpSync(src + "vendor", pub + "vendor", { recursive: true });
console.log("map: tools/edge_map -> public/map.html + public/vendor");
