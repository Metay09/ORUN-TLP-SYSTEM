// 4c: native Android USB serial transport for Adafruit HCI SLIP DFU.
// Needs explicit ORUN device ID: no first-device fallback or generic USB port.
// The bootloader enumerates as 239a:0029; application is 239a:8029.
import type { UsbSerialPlugin } from "@leeskies/capacitor-usb-serial";
import type { DfuSerial } from "../app/firmwareUsb.ts";

type UsbApi = Pick<UsbSerialPlugin, "registerDriver" | "listDevices" | "requestPermission" | "open" |
  "getPortInfo" | "setParameters" | "setDTR" | "read" | "write" | "close">;
const VID = 0x239a, BOOT_PID = 0x0029;
const pause = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

export class DfuUsbPort implements DfuSerial {
  private readonly usb: UsbApi;
  private readonly port: string;
  private constructor(usb: UsbApi, port: string) { this.usb = usb; this.port = port; }

  static async openBootloader(usb: UsbApi, expectedId: string): Promise<DfuUsbPort> {
    if (!/^[0-9A-Fa-f]{16}$/.test(expectedId)) throw new Error("Geçersiz hedef cihaz kimliği.");
    const exact = expectedId.toUpperCase();
    await usb.registerDriver({ vendorId: VID, productId: BOOT_PID, driverType: "CdcAcmSerialDriver" });
    let devices = (await usb.listDevices()).devices.filter((d) => d.vendorId === VID && d.productId === BOOT_PID);
    const matches = devices.filter((d) => d.serialNumber?.toUpperCase() === exact);
    const candidate = matches.length === 1 ? matches[0] : devices.length === 1 ? devices[0] : null;
    if (!candidate) throw new Error("Doğru RAK bootloader'ı tekil olarak seçilemedi.");
    if (candidate.serialNumber && candidate.serialNumber.toUpperCase() !== exact)
      throw new Error("Bootloader'ın USB seri numarası seçilen cihazla farklı.");
    if (!candidate.hasPermission) {
      const { granted } = await usb.requestPermission({ deviceId: candidate.deviceId });
      if (!granted) throw new Error("USB izni verilmedi.");
    }
    devices = (await usb.listDevices()).devices.filter((d) => d.deviceId === candidate.deviceId);
    // A serial-number-less bootloader is unsafe to associate with a live tracker.
    if (devices.length !== 1 || devices[0].serialNumber?.toUpperCase() !== exact)
      throw new Error("Bootloader USB kimliği doğrulanamadı. Yazılım yüklenmedi.");
    const { portId } = await usb.open({ deviceId: candidate.deviceId });
    try {
      const info = await usb.getPortInfo({ portId });
      if (info.deviceId !== candidate.deviceId || info.serialNumber?.toUpperCase() !== exact)
        throw new Error("Açılan USB seri portu hedef cihaza ait değil.");
      await usb.setParameters({ portId, baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none" });
      await usb.setDTR({ portId, value: false });
      await pause(50);
      await usb.setDTR({ portId, value: true });
      return new DfuUsbPort(usb, portId);
    } catch (e) { await usb.close({ portId }).catch(() => undefined); throw e; }
  }

  async write(frame: Uint8Array): Promise<void> {
    // Base64 transport is binary-safe; synchronous driver write, no streaming queue.
    const binary = Array.from(frame, (b) => String.fromCharCode(b)).join("");
    const { bytesWritten } = await this.usb.write({ portId: this.port, data: btoa(binary), timeout: 3000 });
    if (bytesWritten !== frame.length) throw new Error("USB seri yazma eksik kaldı.");
  }

  async read(timeoutMs: number): Promise<Uint8Array> {
    const { data } = await this.usb.read({ portId: this.port, length: 1100,
      timeout: Math.max(1, Math.min(750, Math.floor(timeoutMs))) });
    const binary = atob(data || "");
    return Uint8Array.from(binary, (c) => c.charCodeAt(0));
  }

  async close(): Promise<void> { await this.usb.close({ portId: this.port }); }
}
