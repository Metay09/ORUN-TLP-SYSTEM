// ORUN app domain model (scope doc §14). Screens see only these types; where
// the data came from (phone ledger, sample herd, later the server) is the
// data layer's business. Names follow the architecture documents:
//   Device (Cihaz) — one ORUN device model, services instead of "types";
//   Asset (Varlık) — what a device is attached to (an animal today);
//   Assignment (Atama) — device -> asset with a validity start;
//   Observation (Gözlem) — one located periodic record (core Point);
//   Station (İstasyon) — the Gateway service the phone talks to;
//   AppEvent (Olay) — lifecycle: active -> resolved by a newer measurement.
import type { Point } from "../core/points.ts";
import type { LatLon } from "./geo.ts";

export type Observation = Point;

export type Freshness = "fresh" | "aging" | "stale" | "none";
export type BatteryLevel = "good" | "low" | "critical" | "unknown";
export type Severity = "danger" | "warn" | "info" | "ok";

// Ordered for the herd list: what needs a look first (scope §5).
export type AnimalStatus = "battery-critical" | "lost" | "out" | "silent" | "aging" | "battery-low" | "ok" | "waiting";

export type Sex = "female" | "male" | "";

export interface NoteEntry { at: number; text: string; }

export interface AssetProfile {
  id: string;
  kind: "animal";
  name: string;
  color: string;
  tag: string;        // küpe no
  breed: string;
  sex: Sex;
  birth: string;      // YYYY-MM-DD or ""
  notes: NoteEntry[]; // the animal's own book: health, calving, vaccines
}

export interface Assignment { device: string; asset: string; from: number; }

export interface Animal {
  id: string;               // asset id
  device: string | null;    // currently assigned device (collar)
  profile: AssetProfile;
  name: string;
  named: boolean;           // the owner gave it a name
  initials: string;
  color: string;
  points: Observation[];    // located records, oldest first (this asset only)
  last: Observation | null; // newest by RECORD time
  lastTime: number | null;  // epoch ms of that record
  periodS: number;          // the collar's latest report period
  freshness: Freshness;
  freshRatio: number;       // 1 = just measured .. 0 = stale (the ring)
  status: AnimalStatus;
  seen: string;             // one plain sentence: "8 dk önce görüldü"
  short: string;            // chip text: "8 dk önce"
  battery: BatteryLevel;
  batteryPercent: number | null;
  activeShare: number | null; // last record: share of the period in motion
  zone: string | null;      // name of the zone holding the last position (null: outside all, or no zones)
  outside: boolean;         // confirmed outside every active zone (2 of the last 3 records)
  outSince: number | null;  // when the current outside period began (record time)
  grazing: boolean;         // serbest otlatma is on: outside is allowed, not an alarm
  attention: number;        // sort key, higher first
}

export interface ServiceState {
  key: "location" | "activity" | "battery";
  state: "running" | "silent" | "off";
}

export interface Device {
  id: string;
  short: string;
  asset: string | null;
  install: "animal" | "fixed" | "vehicle" | null;
  services: ServiceState[];
  periodS: number | null;
  lastTime: number | null;     // newest record time
  lastArrival: number | null;  // newest arrival on the phone
  rssi: number | null;
  snr: number | null;
  link: Observation["link"];
  battery: BatteryLevel;
  batteryPercent: number | null;
  batteryMv: number | null;
  records: number;
  medianDelayS: number | null;
}

export type LinkKind = "none" | "ble" | "usb";
export type LinkPhase = "idle" | "connecting" | "connected" | "waiting" | "error";

export interface Station {
  id: string | null;
  link: LinkKind;
  phase: LinkPhase;
  detail: string | null;
  held: number | null;
  capacity: number | null;
  position: LatLon | null;    // where it stands (owner marked it)
}

export interface ServerView {
  configured: boolean;
  url: string;
  token: string;
  auto: boolean;
  busy: boolean;
  uploaded: number | null;
  pending: number | null;
  conflict: number;
  rejected: number;
  lastOk: string | null;
  error: string | null;
}

export type EventKind =
  | "silent" | "lost" | "out" | "battery-low" | "battery-critical" | "still"
  | "station-full" | "server-down" | "link" | "records" | "handoff" | "upload" | "keys" | "note";

export interface AppEvent {
  id: string;
  kind: EventKind;
  severity: Severity;
  asset: string | null;
  device: string | null;
  title: string;
  detail: string;
  at: number;              // when it started (measurement time for record events)
  end: number | null;      // resolved at (null = still active)
  arrivedAt: number | null; // when the evidence reached the phone
  alert: boolean;          // true: a warning; false: a book (Defter) line
}

export type { Zone } from "./zones.ts";
