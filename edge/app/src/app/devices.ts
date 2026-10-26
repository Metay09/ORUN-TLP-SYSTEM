// Device list (scope §8): one ORUN device model. What a device does is told
// by its services, judged from what its records actually carry — not by a
// "tracker/relay/base" type.
import { medianDelayS } from "./activity.ts";
import { batteryOf, batteryPercent, epochMs, freshnessOf, shortId } from "./herd.ts";
import type { Animal, Device, Observation, ServiceState } from "./model.ts";

export function buildDevices(points: Observation[], herd: Animal[], now: number): Device[] {
  const by = new Map<string, Observation[]>();
  for (const p of points) {
    if (!by.has(p.tracker)) by.set(p.tracker, []);
    by.get(p.tracker)!.push(p);
  }
  const assetOf = new Map(herd.filter((a) => a.device).map((a) => [a.device!, a.id]));
  const out: Device[] = [];
  for (const [id, list] of by) {
    const sorted = [...list].sort((a, b) => epochMs(a.time) - epochMs(b.time));
    const last = sorted[sorted.length - 1] ?? null;
    const lastTime = last ? epochMs(last.time) : NaN;
    let lastArrival = -Infinity;
    for (const p of list) if (p.received) lastArrival = Math.max(lastArrival, Date.parse(p.received));
    const fresh = freshnessOf(Number.isFinite(lastTime) ? lastTime : null, last?.period_s ?? 60, now);
    const running = fresh === "fresh" || fresh === "aging";
    const services: ServiceState[] = [
      { key: "location", state: running ? "running" : "silent" },
      { key: "activity", state: sorted.some((p) => p.active_s !== null) ? (running ? "running" : "silent") : "off" },
      { key: "battery", state: sorted.some((p) => p.battery_mv !== null) ? (running ? "running" : "silent") : "off" },
    ];
    const heard = [...sorted].reverse().find((p) => p.rssi !== null) ?? null;
    const bat = batteryOf(last);
    out.push({
      id, short: shortId(id), asset: assetOf.get(id) ?? null, install: assetOf.has(id) ? "animal" : null, services,
      periodS: last?.period_s ?? null,
      lastTime: Number.isFinite(lastTime) ? lastTime : null,
      lastArrival: Number.isFinite(lastArrival) ? lastArrival : null,
      rssi: heard?.rssi ?? null, snr: heard?.snr ?? null, link: last?.link ?? null,
      battery: bat.level, batteryPercent: batteryPercent(last?.battery_mv ?? null), batteryMv: last?.battery_mv ?? null,
      records: list.length, medianDelayS: medianDelayS(list),
    });
  }
  return out.sort((a, b) => (b.lastTime ?? 0) - (a.lastTime ?? 0));
}
