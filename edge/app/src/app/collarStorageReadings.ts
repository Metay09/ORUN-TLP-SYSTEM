// In-memory only: last DIRECT BLE/USB APP STORAGE? reading.
// History store count is NOT a verified LoRa/ACK pending queue.
// Never persisted across installations, shared with the backend or
// interpreted as a live remote measurement.
export interface CollarStorageReading {
  device: string;
  count: number;
  capacity: number;
  at: number;
  via: "ble" | "usb";
}

class CollarStorageReadings {
  private values: Readonly<Record<string, CollarStorageReading>> = {};
  private accountScope: string | null | undefined = undefined;
  private listeners = new Set<() => void>();

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  };

  snapshot = (): Readonly<Record<string, CollarStorageReading>> => this.values;

  /** Keep last reading across tab remounts; clear it only when account changes. */
  setScope(scope: string | null): void {
    if (scope === this.accountScope) return;
    this.accountScope = scope;
    this.clear();
  }

  note(device: string, count: number, capacity: number, at: number,
       via: "ble" | "usb"): void {
    const id = device.toUpperCase();
    if (!/^[0-9A-F]{16}$/.test(id) || !Number.isSafeInteger(count) ||
        !Number.isSafeInteger(capacity) || count < 0 || capacity < count ||
        capacity > 1_000_000 || !Number.isFinite(at) || at <= 0) return;
    const prior = this.values[id];
    if (prior && at < prior.at) return;
    this.values = { ...this.values, [id]: { device: id, count, capacity, at, via } };
    for (const fn of this.listeners) fn();
  }

  clear(): void {
    if (!Object.keys(this.values).length) return;
    this.values = {};
    for (const fn of this.listeners) fn();
  }
}

export const collarStorageReadings = new CollarStorageReadings();
