// 2c: yakındaki ORUN cihazları. Her ORUN cihazı (tasma, gateway, ileride
// sensör/vana) Bluetooth'ta "ORUN-XXXXXXXX" adıyla görünür; XXXXXXXX cihaz
// kimliğinin son 8 hanesidir.
import { BleClient } from "@capacitor-community/bluetooth-le";

export interface NearbyDevice { deviceId: string; name: string; rssi: number | null; }
export type NearbyKind = "gateway" | "tracker" | "unknown";
export interface NearbyView extends NearbyDevice { kind: NearbyKind; label: string; id: string | null; }

const NAME = /^ORUN-([0-9A-F]{8})$/i;

/** Which known device is this? Gateways from the farm list, trackers from the herd. */
export function classifyNearby(
  d: NearbyDevice, gateways: string[], trackers: { id: string; name: string }[],
): NearbyView {
  const m = NAME.exec(d.name);
  const tail = m ? m[1].toUpperCase() : null;
  const gw = tail ? gateways.find((g) => g.toUpperCase().endsWith(tail)) : undefined;
  if (gw) return { ...d, kind: "gateway", id: gw, label: "Gateway " + gw.slice(-4).toUpperCase() };
  const tr = tail ? trackers.find((t) => t.id.toUpperCase().endsWith(tail)) : undefined;
  if (tr) return { ...d, kind: "tracker", id: tr.id, label: tr.name };
  return { ...d, kind: "unknown", id: null, label: d.name || "ORUN cihazı" };
}

/** Strongest first; one row per device. */
export function mergeNearby(list: NearbyDevice[], next: NearbyDevice): NearbyDevice[] {
  const rest = list.filter((x) => x.deviceId !== next.deviceId);
  return [...rest, next].sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
}

/** Scan for `ms`; calls `found` for every ORUN advertisement. */
export async function scanNearby(found: (d: NearbyDevice) => void, ms = 8000): Promise<void> {
  await BleClient.initialize({ androidNeverForLocation: true });
  if (!(await BleClient.isEnabled())) await BleClient.requestEnable();
  await BleClient.requestLEScan({ namePrefix: "ORUN-", allowDuplicates: false }, (r) => {
    const name = r.localName ?? r.device.name ?? "";
    if (NAME.test(name)) found({ deviceId: r.device.deviceId, name, rssi: r.rssi ?? null });
  });
  await new Promise((done) => setTimeout(done, ms));
  await BleClient.stopLEScan();
}

export async function stopNearbyScan(): Promise<void> {
  try { await BleClient.stopLEScan(); } catch { /* not scanning */ }
}
