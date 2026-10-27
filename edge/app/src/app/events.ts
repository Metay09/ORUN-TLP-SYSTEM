// Events (scope §7) derived from what the phone actually holds: gaps in the
// collars' records (silent / may be lost), battery state changes, long
// stillness, station memory filling up, the server unreachable, plus the
// plain-language book (Defter) of links, hand-offs, uploads and notes.
// An event is active until a newer MEASUREMENT resolves it; a late record
// that fills a gap simply removes that gap. "Gördüm" is only the user's
// mark and never closes an event.
import { fmt, tr } from "./i18n/tr.ts";
import { agingLimit, batteryOf, epochMs, lostLimit, periodMs } from "./herd.ts";
import { coverageAt, type Coverage } from "./coverage.ts";
import type { Animal, AppEvent, EventKind, ServerView, Severity, Station } from "./model.ts";
import { outsideEpisodes, type Fence } from "./zones.ts";

const HOUR = 3600_000;
const DAY = 24 * HOUR;
export const STILL_MS = 3 * HOUR;
export const HISTORY_DAYS = 30;

export interface BookLine { at: number; kind: string; text: string; }

export interface EventInput {
  herd: Animal[];
  station: Station;
  server: ServerView;
  journal: BookLine[];
  coverage: Coverage | null;
  now: number;
  fence?: Fence;       // active zones; outside periods become events
  grazing?: boolean;   // serbest otlatma: the open outside period is not an alert
}

function ev(kind: EventKind, severity: Severity, a: Animal | null, at: number, end: number | null,
            title: string, detail: string, arrivedAt: number | null, alert = true): AppEvent {
  return {
    id: `${kind}:${a?.id ?? "-"}:${at}`, kind, severity, asset: a?.id ?? null, device: a?.device ?? null,
    title, detail, at, end, arrivedAt, alert,
  };
}

const arrived = (iso: string | null) => (iso ? Date.parse(iso) : null);

function animalEvents(a: Animal, cov: Coverage | null, now: number, fence?: Fence, grazing = false): AppEvent[] {
  const out: AppEvent[] = [];
  const from = now - HISTORY_DAYS * DAY;
  const pts = a.points.filter((p) => epochMs(p.time) >= from);
  if (!pts.length) return out;

  // Outside every active zone, confirmed by 2 of the last 3 records; closed
  // when it is back inside the same way. While grazing freely, the open
  // period is allowed and raises nothing.
  if (fence && fence.polys.length)
    for (const e of outsideEpisodes(pts, fence, (p) => epochMs(p.time))) {
      if (e.to === null && grazing) continue;
      out.push(ev("out", "danger", a, e.from, e.to, tr.events.out.title(a.name),
        e.to === null ? tr.events.out.detail(fmt.since(e.from, now)) : tr.events.out.back(fmt.duration(e.to - e.from)),
        arrived(pts[e.fromIdx]?.received ?? null)));
    }

  // Silent gaps between records, and the open gap after the newest one.
  for (let i = 1; i < pts.length; i++) {
    const t0 = epochMs(pts[i - 1].time), t1 = epochMs(pts[i].time);
    const T = periodMs(pts[i - 1].period_s), gap = t1 - t0;
    if (gap <= agingLimit(T)) continue;
    const missed = Math.max(1, Math.round(gap / T) - 1);
    const lost = gap > lostLimit(T);
    out.push(ev(lost ? "lost" : "silent", lost ? "danger" : "warn", a, t0, t1,
      lost ? tr.events.lost.title(a.name) : tr.events.silent.title(a.name),
      tr.events.silent.detail(fmt.duration(gap), missed), arrived(pts[i].received)));
  }
  const last = pts[pts.length - 1], tl = epochMs(last.time), T = periodMs(last.period_s);
  if (now - tl > agingLimit(T)) {
    const lost = now - tl > lostLimit(T);
    const missed = Math.max(1, Math.round((now - tl) / T) - 1);
    let detail = lost ? tr.events.lost.detail(fmt.since(tl, now)) : tr.events.silentNow.detail(fmt.since(tl, now), missed);
    const cls = cov ? coverageAt(cov, last.lat, last.lon) : null;
    if (cls === "weak" || cls === "none" || last.link === "cut") detail += ` · ${tr.events.weakHint}`;
    out.push(ev(lost ? "lost" : "silent", lost ? "danger" : "warn", a, tl, null,
      lost ? tr.events.lost.title(a.name) : tr.events.silentNow.title(a.name), detail, arrived(last.received)));
  }

  // Battery: an episode opens when the level drops to low/critical and
  // closes only after a full day back at good (solar collars dip every
  // night); it carries the worst level reached.
  let open: AppEvent | null = null, lastBad = 0;
  for (const p of pts) {
    const b = batteryOf(p);
    if (b.level === "unknown") continue;
    const t = epochMs(p.time);
    if (b.level === "low" || b.level === "critical") {
      lastBad = t;
      if (!open) {
        open = ev("battery-low", "warn", a, t, null, tr.events.batteryLow.title(a.name),
          tr.events.batteryLow.detail(b.percent !== null ? `%${b.percent}` : `${p.battery_mv} mV`), arrived(p.received));
        out.push(open);
      }
      if (b.level === "critical" && open.kind !== "battery-critical") {
        open.kind = "battery-critical"; open.severity = "danger";
        open.title = tr.events.batteryCritical.title(a.name); open.detail = tr.events.batteryCritical.detail;
      }
    } else if (open && t - lastBad >= DAY) {
      open.end = t;
      open = null;
    }
  }

  // Stillness: a run of records with zero movement lasting >= 3 h.
  let runStart: number | null = null, runEnd = 0, runArrived: number | null = null;
  const flush = (active: boolean) => {
    if (runStart !== null && runEnd - runStart >= STILL_MS)
      out.push(ev("still", "warn", a, runStart, active ? null : runEnd, tr.events.still.title(a.name),
        tr.events.still.detail(fmt.duration(runEnd - runStart)), runArrived));
    runStart = null;
  };
  for (const p of pts) {
    const t = epochMs(p.time);
    if (p.active_s === 0 && p.covered_s) {
      if (runStart === null) runStart = t - p.covered_s * 1000;
      runEnd = t;
      runArrived = arrived(p.received);
    } else if (p.active_s !== null) {
      flush(false);
    }
  }
  flush(now - tl <= agingLimit(T));
  return out;
}

export function deriveEvents({ herd, station, server, journal, coverage, now, fence, grazing }: EventInput): AppEvent[] {
  const out: AppEvent[] = [];
  for (const a of herd) out.push(...animalEvents(a, coverage, now, fence, grazing));

  // Station and server: live conditions (stable ids so "Gördüm" sticks).
  if (station.held !== null && station.capacity && station.held / station.capacity >= 0.8) {
    const e = ev("station-full", "warn", null, now, null, tr.events.stationFull.title,
      tr.events.stationFull.detail(station.held, station.capacity), null);
    e.id = `station-full:${station.id ?? "-"}`;
    out.push(e);
  }
  if (server.configured && server.error && (server.pending ?? 0) > 0) {
    const e = ev("server-down", "warn", null, now, null, tr.events.serverDown.title,
      tr.events.serverDown.detail(server.pending ?? 0), null);
    e.id = "server-down";
    out.push(e);
  }

  for (const j of journal) {
    const kind = (["link", "records", "handoff", "upload", "keys"].includes(j.kind) ? j.kind : "link") as EventKind;
    const e = ev(kind, j.kind === "warn" ? "warn" : "info", null, j.at, j.at, j.text, "", null, false);
    e.id = `book:${j.at}:${j.text}`;
    out.push(e);
  }
  for (const a of herd)
    for (const n of a.profile.notes) {
      const e = ev("note", "info", a, n.at, n.at, a.name, n.text, null, false);
      e.id = `note:${a.id}:${n.at}`;
      out.push(e);
    }
  return out.sort((x, y) => y.at - x.at);
}

export const isActive = (e: AppEvent) => e.alert && e.end === null;

export function activeCount(events: AppEvent[], seen: Set<string>): number {
  return events.filter((e) => isActive(e) && !seen.has(e.id)).length;
}
