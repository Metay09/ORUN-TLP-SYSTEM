// 1e: tek seferlik USB satır konsolu (tasma kurulumu için).
// İstasyon USB bağlantısıyla aynı anda açılmaz; iş bitince kapanır.
import { UsbSerial } from "@leeskies/capacitor-usb-serial";
import type { PluginListenerHandle } from "@capacitor/core";
import type { LineConsole } from "../app/addTracker.ts";

const RAK_VID = 0x239a;
const RAK_PIDS = [0x8029, 0x802a] as const;

export class UsbConsole implements LineConsole {
  private portId: string | null = null;
  private buf = "";
  private lines: string[] = [];
  private waiters: (() => void)[] = [];
  private listeners: PluginListenerHandle[] = [];
  private decoder = new TextDecoder();

  static async open(exactDeviceId?: string): Promise<UsbConsole> {
    const con = new UsbConsole();
    for (const productId of RAK_PIDS)
      await UsbSerial.registerDriver({ vendorId: RAK_VID, productId, driverType: "CdcAcmSerialDriver" });
    const { devices } = await UsbSerial.listDevices();
    const isOrun = (d: typeof devices[number]) => d.vendorId === RAK_VID &&
      RAK_PIDS.includes(d.productId as (typeof RAK_PIDS)[number]);
    const dev = exactDeviceId
      ? devices.find((d) => d.deviceId === exactDeviceId && isOrun(d))
      : devices.find(isOrun);
    if (!dev) throw new Error("USB'de cihaz yok (OTG kablosu takılı mı?)");
    if (!dev.hasPermission) {
      const { granted } = await UsbSerial.requestPermission({ deviceId: dev.deviceId });
      if (!granted) throw new Error("USB izni verilmedi");
    }
    const { portId } = await UsbSerial.open({ deviceId: dev.deviceId });
    con.portId = portId;
    await UsbSerial.setParameters({ portId, baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none" });
    try { await UsbSerial.setDTR({ portId, value: true }); } catch { /* optional */ }
    con.listeners.push(await UsbSerial.addListener("data", (e) => {
      if (e.portId !== con.portId) return;
      const s = atob(e.data);
      const bytes = new Uint8Array(s.length);
      for (let i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i);
      con.onText(con.decoder.decode(bytes, { stream: true }));
    }));
    await UsbSerial.startReading({ portId });
    await new Promise((r) => setTimeout(r, 400));   // açılış yazıları gelsin
    con.lines = [];
    return con;
  }

  private onText(text: string): void {
    this.buf += text;
    let i: number;
    while ((i = this.buf.indexOf("\n")) >= 0) {
      this.lines.push(this.buf.slice(0, i).replace(/\r$/, ""));
      this.buf = this.buf.slice(i + 1);
    }
    if (this.buf.length > 4096) this.buf = "";
    for (const w of this.waiters.splice(0)) w();
  }

  async send(line: string): Promise<void> {
    if (!this.portId) throw new Error("USB kapalı");
    await UsbSerial.write({ portId: this.portId, data: btoa(line + "\n"), timeout: 1000 });
  }

  async waitFor<T>(match: (line: string) => T | null, ms: number): Promise<T | null> {
    const end = Date.now() + ms;
    for (;;) {
      while (this.lines.length) {
        const got = match(this.lines.shift() as string);
        if (got !== null) return got;
      }
      const left = end - Date.now();
      if (left <= 0) return null;
      await new Promise<void>((resolve) => {
        const t = setTimeout(resolve, left);
        this.waiters.push(() => { clearTimeout(t); resolve(); });
      });
    }
  }

  async close(): Promise<void> {
    for (const l of this.listeners) { try { await l.remove(); } catch { /* gone */ } }
    this.listeners = [];
    if (this.portId) {
      const p = this.portId;
      this.portId = null;
      try { await UsbSerial.stopReading({ portId: p }); } catch { /* closed */ }
      try { await UsbSerial.close({ portId: p }); } catch { /* closed */ }
    }
  }
}
