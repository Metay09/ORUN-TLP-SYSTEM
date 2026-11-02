// 3a: her tasmaya hangi alanların gideceği ve tasmadaki alanların durumu.
import { fenceDigest, polygonsOf } from "./fenceUsb.ts";
import { fenceState, type FenceRecord, type FenceState } from "./trackerFences.ts";
import { zonePlanForDevice, type TargetBook } from "./zoneTargets.ts";
import type { Zone } from "./zones.ts";

export interface TrackerFence {
  device: string;           // as the herd knows it
  name: string;
  zones: Zone[];            // what it should hold
  errors: string[];         // the set the tracker would refuse
  digest: string | null;    // null: no zone for it
  record: FenceRecord | null;
  state: FenceState;
}

export function trackerFencePlans(book: TargetBook, zones: Zone[],
                                  trackers: { device: string; name: string }[],
                                  records: Record<string, FenceRecord>): TrackerFence[] {
  const known = trackers.map((t) => t.device);
  return trackers.map((t) => {
    const plan = zonePlanForDevice(book, zones, t.device, known);
    const digest = plan.zones.length ? fenceDigest(polygonsOf(plan.zones)) : null;
    const record = records[t.device.toUpperCase()] ?? null;
    return { device: t.device, name: t.name, zones: plan.zones, errors: plan.errors, digest, record,
             state: fenceState(record, digest) };
  });
}

/** Trackers whose zones need sending (never sent, or changed since). */
export function needsSending(plans: TrackerFence[]): TrackerFence[] {
  return plans.filter((p) => p.state === "never" || p.state === "stale");
}

/**
 * Trackers whose zones differ from what they hold and are not already queued
 * with exactly these zones (`queued`: device → digest of its open operation).
 */
export function toApply(plans: TrackerFence[], queued: Map<string, string> = new Map(), only?: string): TrackerFence[] {
  return plans.filter((p) => (p.state === "stale" || p.state === "never") && !p.errors.length && p.zones.length > 0 &&
    queued.get(p.device.toUpperCase()) !== p.digest &&
    (!only || p.device.toUpperCase() === only.toUpperCase()));
}
