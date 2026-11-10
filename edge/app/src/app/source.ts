// The data layer seam. The phone ledger (Controller) implements EdgeSource;
// screens only see this interface.
import type { PointsResult } from "../core/points.ts";
import type { Snapshot } from "./controller.ts";
import type { LinkPhase, ServerView, Station } from "./model.ts";
import type { LatLon } from "./geo.ts";

export interface EdgeSource {
  readonly kind: "real";
  snap: Snapshot;
  subscribe(fn: () => void): () => void;
  points(): Promise<PointsResult | null>;
  connectBle(pick?: boolean, deviceId?: string): Promise<void>;
  connectUsb(): Promise<void>;
  disconnect(): Promise<void>;
  saveServer(url: string, token: string, auto: boolean): void;
  sync(manual: boolean): Promise<void>;
}

export function stationOf(s: Snapshot, places: Record<string, LatLon>): Station {
  const [held, capacity] = s.gatewayHeld ? s.gatewayHeld.split("/").map(Number) : [NaN, NaN];
  const phase: LinkPhase = s.state === "searching" ? "connecting" : s.state;
  return {
    id: s.gateway, link: s.link, phase: s.link === "none" ? "idle" : phase, detail: s.detail,
    held: Number.isFinite(held) ? held : null, capacity: Number.isFinite(capacity) ? capacity : null,
    position: s.gateway ? places[s.gateway] ?? null : null,
  };
}

export function serverOf(s: Snapshot): ServerView {
  const sv = s.server, c = sv.counts;
  return {
    configured: Boolean(sv.url && sv.token), url: sv.url, token: sv.token, auto: sv.auto, busy: sv.busy,
    uploaded: c?.uploaded ?? null, pending: c?.pending ?? null, conflict: c?.conflict ?? 0, rejected: c?.rejected ?? 0,
    lastOk: sv.lastOk, error: sv.error,
  };
}
