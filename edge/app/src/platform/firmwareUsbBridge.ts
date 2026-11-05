// 4c: Android USB entry/return for Adafruit RAK4630 serial DFU.
// Every port is matched by USB serial number AND the ORUN application's BENCH ID.
// Never fall back to the first available USB device.
import { UsbSerial } from "@leeskies/capacitor-usb-serial";
import { readFirmwareEvidence, type FirmwareEvidence } from "../app/firmwareSafety.ts";
import { DfuUsbPort } from "./dfuUsbPort.ts";
import { UsbConsole } from "./usbConsole.ts";

const VID = 0x239a, APP_PIDS = [0x8029, 0x802a] as const;
const sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));
const assertId = (id: string) => {
  if (!/^[0-9A-F]{16}$/.test(id)) throw new Error("Geçersiz RAK cihaz kimliği.");
};
type SerialInfo = { deviceId: string; vendorId: number; productId: number; serialNumber: string | null; hasPermission: boolean };

/** A uniquely matched application, including when serial needs USB permission to be exposed. */
async function exactApplication(id: string): Promise<SerialInfo> {
  assertId(id);
  for (const productId of APP_PIDS)
    await UsbSerial.registerDriver({ vendorId: VID, productId, driverType: "CdcAcmSerialDriver" });
  const list = (await UsbSerial.listDevices()).devices.filter((d) => d.vendorId === VID && APP_PIDS.includes(d.productId as (typeof APP_PIDS)[number]));
  const matches = list.filter((d) => d.serialNumber?.toUpperCase() === id);
  if (matches.length > 1 || (matches.length === 0 && list.length !== 1))
    throw new Error("Uygulama USB portu bulunamadı ya da hedef belirsiz.");
  const dev = matches[0] ?? list[0];
  if (dev.serialNumber && dev.serialNumber.toUpperCase() !== id)
    throw new Error("USB seri numarası hedef ORUN cihazıyla eşleşmedi.");
  if (!dev.hasPermission) {
    const { granted } = await UsbSerial.requestPermission({ deviceId: dev.deviceId });
    if (!granted) throw new Error("USB izni verilmedi.");
  }
  const refreshed = (await UsbSerial.listDevices()).devices.find((d) => d.deviceId === dev.deviceId);
  if (!refreshed || refreshed.vendorId !== VID || !APP_PIDS.includes(refreshed.productId as (typeof APP_PIDS)[number]) ||
      refreshed.serialNumber?.toUpperCase() !== id)
    throw new Error("USB cihaz kimliği doğrulanamadı.");
  return refreshed;
}

export async function readOrunApplication(id: string, retryForMs = 0): Promise<FirmwareEvidence> {
  assertId(id);
  const deadline = Date.now() + retryForMs;
  let last: unknown = new Error("Cihaz henüz açılmadı.");
  do {
    let con: UsbConsole | null = null;
    try {
      const app = await exactApplication(id);
      con = await UsbConsole.open(app.deviceId);
      const evidence = await readFirmwareEvidence(con);
      if (evidence.id !== id) throw new Error("Cihazın BENCH kimliği USB kimliğiyle uyuşmuyor.");
      return evidence;
    } catch (e) { last = e; }
    finally { if (con) await con.close().catch(() => undefined); }
    if (Date.now() >= deadline) break;
    await sleep(Math.min(1100, deadline - Date.now()));
  } while (true);
  throw last;
}

/**
 * Trigger Adafruit's standard 1200-baud CDC touch. Closing this port should
 * make the bootloader enumerate as 239A:0029. Never send flash commands here.
 */
export async function touchOrunBootloader(id: string): Promise<void> {
  const dev = await exactApplication(id);
  const { portId } = await UsbSerial.open({ deviceId: dev.deviceId });
  try {
    const info = await UsbSerial.getPortInfo({ portId });
    if (info.deviceId !== dev.deviceId || info.serialNumber?.toUpperCase() !== id)
      throw new Error("Açılan USB portu yanlış cihazda.");
    await UsbSerial.setParameters({ portId, baudRate: 1200, dataBits: 8, stopBits: 1, parity: "none" });
    // Adafruit --touch 1200 opens and closes the port at that baud rate;
    // the close triggers bootloader entry. Do not pulse DTR separately.
    await sleep(100);
  } finally { await UsbSerial.close({ portId }).catch(() => undefined); }
}

/** Polls only during bootloader enumeration; never retries a mutating packet. */
export async function enterOrunBootloader(id: string): Promise<DfuUsbPort> {
  await touchOrunBootloader(id);
  const deadline = Date.now() + 15_000;
  let last: unknown = null;
  for (;;) {
    try { return await DfuUsbPort.openBootloader(UsbSerial, id); }
    catch (e) {
      last = e;
      const message = e instanceof Error ? e.message : String(e);
      if (message.includes("farklı") || message.includes("doğrulanamadı") ||
          message.includes("izni verilmedi")) throw e;
      if (Date.now() >= deadline) break;
      await sleep(600);
    }
  }
  throw new Error("Bootloader'a geçilemedi; cihaza yazılım gönderilmedi. " +
    (last instanceof Error ? last.message : ""));
}

/** Reboot may temporarily report an unready store. Retry ONLY read-only checks. */
export function readAfterOrunUpdate(id: string): Promise<FirmwareEvidence> {
  return readOrunApplication(id, 60_000);
}
