// DEV ONLY (not in the app build): the UI with sample data, for screenshots.
import { createRoot } from "react-dom/client";
import "@fontsource/figtree/latin-400.css";
import "@fontsource/figtree/latin-ext-400.css";
import "@fontsource/figtree/latin-500.css";
import "@fontsource/figtree/latin-ext-500.css";
import "@fontsource/figtree/latin-600.css";
import "@fontsource/figtree/latin-ext-600.css";
import "./ui/styles.css";
import type { Point } from "./core/points.ts";

const now = Date.now();
const iso = (ms: number) => new Date(ms).toISOString().replace(/\.\d+Z$/, "Z");
function track(id: string, lat: number, lon: number, lastAgoMin: number, n: number, extra: Partial<Point> = {}): Point[] {
  const out: Point[] = [];
  for (let i = n - 1; i >= 0; i--) {
    const t = now - (lastAgoMin + i * 20) * 60000;
    out.push({ tracker: id, seq: n - i, time: iso(t), lat: lat + Math.sin(i / 3) * 0.002 + i * 0.0003, lon: lon + Math.cos(i / 4) * 0.002,
      sats: 10, hdop: 0.9, fix_age_s: 0, battery_mv: 4065, battery: "NORMAL", active_s: 40, covered_s: 59, source: "GNSS",
      period_s: 59, rssi: -90, snr: 7, received: iso(t + 2000), delay_s: 2, link: "live", ...extra });
  }
  return out;
}
const points = [
  ...track("0E8ADE7E71531AA3", 37.6107, 28.0546, 1, 60),
  ...track("0E8ADE7E71530B21", 37.6142, 28.0598, 6, 40, { active_s: 3 }),
  ...track("0E8ADE7E71537C40", 37.6071, 28.0489, 130, 30, { battery_mv: 3610, battery: "LOW" }),
  ...track("0E8ADE7E7153D901", 37.6119, 28.0621, 35, 20),
];
try {
  localStorage.setItem("orun.edge.animals", JSON.stringify({ "0E8ADE7E71531AA3": { name: "Sarıkız" }, "0E8ADE7E71530B21": { name: "Karabaş" } }));
  localStorage.setItem("orun.edge.journal", "[]");
} catch { /* */ }
const journal = [
  { at: now - 26 * 3600e3, kind: "upload", text: "214 kayıt sunucuya gönderildi", n: 214 },
  { at: now - 50 * 60e3, kind: "link", text: "İstasyona bağlanıldı (Bluetooth)" },
  { at: now - 49 * 60e3, kind: "records", text: "38 yeni kayıt istasyondan alındı", n: 38 },
  { at: now - 48 * 60e3, kind: "handoff", text: "38 kayıt telefona kalıcı yazıldı · istasyonda yer açıldı", n: 38 },
  { at: now - 47 * 60e3, kind: "upload", text: "38 kayıt sunucuya gönderildi", n: 38 },
  { at: now - 5 * 60e3, kind: "warn", text: "Sunucuya ulaşılamadı · 4 kayıt telefonda saklı, sonra gönderilecek" },
];
const snap = {
  ready: true, error: null, link: "ble", state: "connected", detail: null,
  counts: { STORED: 38, DUPLICATE: 2, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 }, records: 641, handedOff: 38, rejected: 0, pending: 0,
  gateway: "09A462BD4B275BA5", gatewayHeld: "0/1056", storage: "SQLite journal=wal synchronous=2",
  log: ["14:20:01 [ble] bağlandı", "14:20:02 EDGE HELD v1 ..."],
  server: { url: "http://100.114.36.9:8090", token: "x", auto: true, busy: false, counts: { uploaded: 637, pending: 4, conflict: 0, rejected: 0 }, lastOk: "14:20:31", error: "ağ yok" },
  pointsVersion: 1, journal,
};
const fake = {
  snap, subscribe: () => () => {}, points: async () => ({ count: points.length, skipped: 0, now: iso(now), points }),
  connectBle: async () => {}, connectUsb: async () => {}, disconnect: async () => {}, saveServer: () => {}, sync: async () => {},
};
// App is imported after the sample names are in storage.
const { App } = await import("./ui/App.tsx");
// eslint-disable-next-line @typescript-eslint/no-explicit-any
createRoot(document.getElementById("root")!).render(<App controller={fake as any} />);
