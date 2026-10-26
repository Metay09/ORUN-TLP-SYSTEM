// Development only (npm run dev, "?empty"): a phone with an empty ledger and
// no station, to check the empty states without a device.
import type { Snapshot } from "./controller.ts";
import type { EdgeSource } from "./source.ts";

export function emptySource(): EdgeSource {
  const snap: Snapshot = {
    ready: true, error: null, link: "none", state: "idle", detail: null,
    counts: { STORED: 0, DUPLICATE: 0, REJECTED: 0, CONFLICT: 0, UNCERTAIN: 0 },
    records: 0, handedOff: 0, rejected: 0, pending: 0, gateway: null, gatewayHeld: null, storage: "",
    log: [], server: { url: "", token: "", auto: true, busy: false, counts: null, lastOk: null, error: null },
    pointsVersion: 0, journal: [],
  };
  return {
    kind: "real", snap,
    subscribe: () => () => {},
    points: async () => ({ count: 0, skipped: 0, now: new Date().toISOString(), points: [] }),
    connectBle: async () => {}, connectUsb: async () => {}, disconnect: async () => {},
    saveServer: () => {}, sync: async () => {},
  };
}
