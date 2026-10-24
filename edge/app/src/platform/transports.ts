// The two ways the phone reaches a Gateway. Both feed the same EdgeCore.
//  - BLE: the ORUN Edge GATT service (SF5L6), 20-byte frames, indications in,
//    write-with-response out. Needs the SF5L6 Gateway firmware.
//  - USB: the Gateway's serial lines over USB OTG (CDC-ACM), the same text
//    protocol as the PC tool (SF5L1/SF5L5).
import { BleClient, ConnectionPriority } from "@capacitor-community/bluetooth-le";
import { UsbSerial } from "@leeskies/capacitor-usb-serial";
import type { PluginListenerHandle } from "@capacitor/core";
import type { EdgeCore } from "../core/edge.ts";
import {
  EDGE_SERVICE_UUID, EDGE_TO_EDGE_UUID, EDGE_TO_GATEWAY_UUID, frames, Reassembler,
} from "../core/edgeLink.ts";

export type LinkState = "idle" | "searching" | "connecting" | "connected" | "waiting";

export interface LinkEvents {
  log(line: string): void;
  state(s: LinkState, detail?: string): void;
  changed(): void;      // ledger / counters changed: refresh the UI
}

const ALIVE_MS = 15000;
const POLL_MS = 100;
const RETRY_MS = 5000;
const LAST_BLE = "orun.edge.lastBle";

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

// ---- BLE -------------------------------------------------------------------
export class BleLink {
  private deviceId: string | null = null;
  private running = false;
  private connected = false;
  private writing = false;
  private rx = new Reassembler();
  private timers: ReturnType<typeof setInterval>[] = [];
  private messageId = 0;
  private core: EdgeCore;
  private ev: LinkEvents;

  constructor(core: EdgeCore, ev: LinkEvents) { this.core = core; this.ev = ev; }

  /** pick=true shows the system device picker even if a Gateway is remembered. */
  async start(pick = false): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      await BleClient.initialize({ androidNeverForLocation: true });
      if (!(await BleClient.isEnabled())) await BleClient.requestEnable();
      let remembered: string | null = null;
      try { remembered = localStorage.getItem(LAST_BLE); } catch { /* none */ }
      if (pick || !remembered) {
        this.ev.state("searching");
        const device = await BleClient.requestDevice({ services: [EDGE_SERVICE_UUID] });
        this.deviceId = device.deviceId;
        try { localStorage.setItem(LAST_BLE, device.deviceId); } catch { /* fine */ }
      } else {
        this.deviceId = remembered;
      }
      await this.connect();
    } catch (e) {
      this.running = false;
      this.ev.state("idle", errorText(e));
      this.ev.log(`[BLE] başlatılamadı: ${errorText(e)}`);
    }
  }

  async stop(): Promise<void> {
    this.running = false;
    this.clearTimers();
    if (this.deviceId && this.connected) {
      try { await BleClient.disconnect(this.deviceId); } catch { /* gone */ }
    }
    if (this.connected) await this.dropped(false);
    this.ev.state("idle");
  }

  private clearTimers(): void {
    for (const t of this.timers) clearInterval(t);
    this.timers = [];
  }

  private async connect(): Promise<void> {
    if (!this.running || !this.deviceId) return;
    const id = this.deviceId;
    this.ev.state("connecting");
    try {
      await BleClient.connect(id, () => { void this.dropped(true); }, { timeout: 20000 });
      try { await BleClient.requestConnectionPriority(id, ConnectionPriority.CONNECTION_PRIORITY_HIGH); } catch { /* optional */ }
      this.rx.reset();
      this.connected = true;
      await this.core.sessionAlive("ble");
      await BleClient.startNotifications(id, EDGE_SERVICE_UUID, EDGE_TO_EDGE_UUID, (v) => {
        const frame = new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice();
        const msg = this.rx.push(frame, Date.now());
        if (msg) void this.core.handleMessage(msg).then((out) => { if (out) this.ev.log(out); this.ev.changed(); });
      });
      this.ev.state("connected", id);
      this.ev.log(`[BLE] bağlandı: ${id}`);
      this.timers.push(setInterval(() => { void this.pump(); }, POLL_MS));
      this.timers.push(setInterval(() => { void this.core.sessionAlive("ble"); }, ALIVE_MS));
    } catch (e) {
      this.ev.log(`[BLE] bağlanamadı: ${errorText(e)}; ${RETRY_MS / 1000} sn sonra tekrar`);
      this.ev.state("waiting", errorText(e));
      setTimeout(() => { void this.connect(); }, RETRY_MS);
    }
  }

  private async dropped(retry: boolean): Promise<void> {
    if (!this.connected) return;
    this.connected = false;
    this.clearTimers();
    await this.core.sessionEnd();
    this.ev.changed();
    if (retry && this.running) {
      this.ev.log("[BLE] bağlantı koptu; yeniden denenecek");
      this.ev.state("waiting", "bağlantı koptu");
      setTimeout(() => { void this.connect(); }, RETRY_MS);
    }
  }

  // Sends the next confirmed hand-off, one frame at a time (write with response).
  private async pump(): Promise<void> {
    this.rx.poll(Date.now());
    if (this.writing || !this.connected || !this.deviceId) return;
    const accept = this.core.nextAcceptMessage();
    if (!accept) return;
    this.writing = true;
    try {
      this.messageId = (this.messageId + 1) & 0xff;
      for (const f of frames(accept, this.messageId))
        await BleClient.write(this.deviceId, EDGE_SERVICE_UUID, EDGE_TO_GATEWAY_UUID,
                              new DataView(f.buffer, f.byteOffset, f.byteLength));
    } catch (e) {
      // The accept stays in flight; the core resends it after 3 s of silence.
      this.ev.log(`[BLE] yazılamadı: ${errorText(e)}`);
    } finally {
      this.writing = false;
    }
  }
}

// ---- USB -------------------------------------------------------------------
const RAK_VID = 0x239a;   // RAK4631 (Adafruit nRF52 core), application USB
const RAK_PID = 0x8029;

function toBase64(text: string): string {
  return btoa(text);      // accept lines are ASCII
}
function fromBase64(b64: string): Uint8Array {
  const s = atob(b64);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

export class UsbLink {
  private portId: string | null = null;
  private running = false;
  private buf = "";
  private decoder = new TextDecoder();
  private listeners: PluginListenerHandle[] = [];
  private timers: ReturnType<typeof setInterval>[] = [];
  private writing = false;
  private core: EdgeCore;
  private ev: LinkEvents;

  constructor(core: EdgeCore, ev: LinkEvents) { this.core = core; this.ev = ev; }

  async start(): Promise<void> {
    if (this.running) return;
    this.running = true;
    this.ev.state("searching");
    try {
      await UsbSerial.registerDriver({ vendorId: RAK_VID, productId: RAK_PID, driverType: "CdcAcmSerialDriver" });
      const { devices } = await UsbSerial.listDevices();
      const dev = devices.find((d) => d.vendorId === RAK_VID) ?? devices[0];
      if (!dev) throw new Error("USB'de gateway yok (OTG kablosu takılı mı?)");
      if (!dev.hasPermission) {
        const { granted } = await UsbSerial.requestPermission({ deviceId: dev.deviceId });
        if (!granted) throw new Error("USB izni verilmedi");
      }
      this.ev.state("connecting");
      const { portId } = await UsbSerial.open({ deviceId: dev.deviceId });
      this.portId = portId;
      await UsbSerial.setParameters({ portId, baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none" });
      // The nRF52 USB serial only sends while the host holds DTR.
      try { await UsbSerial.setDTR({ portId, value: true }); } catch { /* driver without DTR */ }
      try { await UsbSerial.setRTS({ portId, value: true }); } catch { /* optional */ }
      this.listeners.push(await UsbSerial.addListener("data", (e) => {
        if (e.portId === this.portId) this.onBytes(fromBase64(e.data));
      }));
      this.listeners.push(await UsbSerial.addListener("error", (e) => {
        if (e.portId === this.portId) { this.ev.log(`[USB] hata: ${e.message}`); void this.stop(); }
      }));
      this.listeners.push(await UsbSerial.addListener("detached", (e) => {
        if (e.deviceId === dev.deviceId) { this.ev.log("[USB] kablo çıkarıldı"); void this.stop(); }
      }));
      await UsbSerial.startReading({ portId });
      await this.core.sessionAlive("usb");
      this.ev.state("connected", dev.deviceName);
      this.ev.log(`[USB] bağlandı: ${dev.deviceName}; tutulan kayıtlar isteniyor`);
      await this.writeLine("BENCH GW DUMP");
      this.timers.push(setInterval(() => { void this.pump(); }, POLL_MS));
      this.timers.push(setInterval(() => { void this.core.sessionAlive("usb"); }, ALIVE_MS));
    } catch (e) {
      this.ev.log(`[USB] bağlanamadı: ${errorText(e)}`);
      await this.stop(errorText(e));
    }
  }

  async stop(reason?: string): Promise<void> {
    const wasOpen = this.portId !== null;
    this.running = false;
    for (const t of this.timers) clearInterval(t);
    this.timers = [];
    for (const l of this.listeners) { try { await l.remove(); } catch { /* gone */ } }
    this.listeners = [];
    if (this.portId) {
      const p = this.portId;
      this.portId = null;
      try { await UsbSerial.stopReading({ portId: p }); } catch { /* closed */ }
      try { await UsbSerial.close({ portId: p }); } catch { /* closed */ }
    }
    if (wasOpen) await this.core.sessionEnd();
    this.buf = "";
    this.ev.state("idle", reason);
    this.ev.changed();
  }

  private onBytes(bytes: Uint8Array): void {
    this.buf += this.decoder.decode(bytes, { stream: true });
    let i: number;
    while ((i = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, i).replace(/\r$/, "");
      this.buf = this.buf.slice(i + 1);
      void this.core.handleLine(line).then((out) => { if (out) this.ev.log(out); this.ev.changed(); });
    }
    if (this.buf.length > 4096) this.buf = "";   // garbage without newlines
  }

  private async writeLine(line: string): Promise<void> {
    if (!this.portId) return;
    await UsbSerial.write({ portId: this.portId, data: toBase64(line + "\n"), timeout: 1000 });
  }

  private async pump(): Promise<void> {
    if (this.writing || !this.portId) return;
    const line = this.core.nextAcceptLine();
    if (!line) return;
    this.writing = true;
    try {
      await this.writeLine(line);
    } catch (e) {
      this.ev.log(`[USB] yazılamadı: ${errorText(e)}`);
    } finally {
      this.writing = false;
    }
  }
}
