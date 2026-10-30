// Canonical server zone previews for the Android account pilot.
// These are read-only, account-scoped drafts, NEVER device-applied fences.
// Do not import into orun.app.zones.v1 or calculate live geofence alarms.
import { areaM2, validateRing, type LonLat } from "./zones.ts";
import type { SharedMirrorSnapshot } from "./accountSharedMirror.ts";

const ZONE_KEY = /^zone:([A-Za-z0-9._-]{1,128})$/;
const MAX_VISIBLE = 50;

export interface SharedZonePreview {
  id: string;
  name: string;
  active: boolean;
  hectares: number;
  vertices: number;
  revision: number;
}

export interface SharedZoneList {
  entries: SharedZonePreview[];
  total: number;
  invalid: number;
}

/** Projection for display only. Malformed canonical bodies are skipped, not repaired. */
export function sharedZonePreviews(
  snapshot: Pick<SharedMirrorSnapshot, "resources">,
): SharedZoneList {
  const entries: SharedZonePreview[] = [];
  let invalid = 0;

  for (const [key, record] of Object.entries(snapshot.resources)) {
    if (!key.startsWith("zone:") || record.deleted) continue;
    const match = ZONE_KEY.exec(key);
    const body = record.body;
    if (!match || !body || typeof body.name !== "string" ||
        !body.name.trim() || body.name.trim().length > 40 ||
        typeof body.active !== "boolean" ||
        !Number.isSafeInteger(record.revision) || record.revision < 1 ||
        !Array.isArray(body.ring_e7) ||
        body.ring_e7.length < 3 || body.ring_e7.length > 64) {
      invalid++;
      continue;
    }

    // Backend canonical E7 is [latitude, longitude], whereas UI is [lon, lat].
    const ring: LonLat[] = [];
    let valid = true;
    for (const pair of body.ring_e7) {
      if (!Array.isArray(pair) || pair.length !== 2 ||
          !Number.isSafeInteger(pair[0]) || !Number.isSafeInteger(pair[1]) ||
          Math.abs(pair[0]) > 900_000_000 ||
          Math.abs(pair[1]) > 1_800_000_000) {
        valid = false;
        break;
      }
      ring.push([pair[1] / 1e7, pair[0] / 1e7]);
    }
    if (!valid || validateRing(ring) !== null) {
      invalid++;
      continue;
    }

    const hectares = areaM2(ring) / 10_000;
    if (!Number.isFinite(hectares)) {
      invalid++;
      continue;
    }
    entries.push({
      id: match[1],
      name: body.name.trim(),
      active: body.active,
      hectares,
      vertices: ring.length,
      revision: record.revision,
    });
  }

  entries.sort((a, b) => a.name.localeCompare(b.name, "tr") ||
    a.id.localeCompare(b.id));
  return { entries: entries.slice(0, MAX_VISIBLE),
           total: entries.length, invalid };
}
