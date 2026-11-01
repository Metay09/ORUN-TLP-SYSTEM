// 1d/1e: telefona USB ile bağlı tasmayı hesaba ekle.
// Tasma kendi raporunu verir (BENCH?), sunucu çiftliğin her gateway'i için
// bir kurulum satırı döner, satırlar tasmaya yazılır, tasma raporu ile onaylanır.
// Kurulum satırı tasmanın kök anahtarını taşır: yalnız USB'ye yazılır,
// hiçbir yerde saklanmaz ya da gösterilmez.

export interface LineConsole {
  send(line: string): Promise<void>;
  /** Resolves with the first line `match` accepts, or null after `ms`. */
  waitFor<T>(match: (line: string) => T | null, ms: number): Promise<T | null>;
}
export interface SetupServer {
  devicePost(path: "/api/v1/devices/tracker/prepare" | "/api/v1/devices/tracker/confirm",
             body: unknown): Promise<Record<string, unknown>>;
}
export interface TrackerReport {
  device: string; credential: string; epoch: number;
  granted: string; generation: number; floor: number;
}

const TRACKER = /BENCH TRACKER id=([0-9A-F]{16}) .*granted_gateway=([0-9A-F]{16}|none)/;
const CREDENTIAL = /BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) grant_generation=([0-9A-F]{8}) custody_floor=([0-9A-F]{8})/;
const GATEWAY = /BENCH GATEWAY id=([0-9A-F]{16}) /;
const SETUP = /BENCH (SETUP PASS|SETUP FAIL[^\n]*|PROVISION (?:FAIL|rejected)[^\n]*)/;

/** Ask the device who it is. Gateways are added by their bridge, not here. */
export async function readTrackerReport(con: LineConsole): Promise<TrackerReport> {
  await con.send("BENCH?");
  let cred: RegExpMatchArray | null = null;
  const tracker = await con.waitFor((line) => {
    if (GATEWAY.test(line)) return "gateway" as const;
    const c = line.match(CREDENTIAL);
    if (c) cred = c;
    const t = line.match(TRACKER);
    return t && cred ? t : null;
  }, 6000);
  if (tracker === "gateway")
    throw new Error("Bu bir gateway. Gateway'ler köprüye (PC Edge ya da telefon) bağlanınca çiftliğe kendiliğinden eklenir.");
  if (!tracker || !cred)
    throw new Error("Tasma yanıt vermedi. Kabloyu ve tasma yazılımını kontrol et.");
  const c = cred as RegExpMatchArray;
  return {
    device: tracker[1], granted: tracker[2], credential: c[1],
    epoch: parseInt(c[2], 16), generation: parseInt(c[3], 16), floor: parseInt(c[4], 16),
  };
}

export type AddStep = "okunuyor" | "hazırlanıyor" | "yazılıyor" | "onaylanıyor";

export async function addTrackerOverUsb(
  con: LineConsole, server: SetupServer, onStep: (s: AddStep, detail?: string) => void = () => {},
): Promise<{ device: string; gateways: string[]; added: number }> {
  onStep("okunuyor");
  const before = await readTrackerReport(con);
  onStep("hazırlanıyor", before.device);
  const prep = await server.devicePost("/api/v1/devices/tracker/prepare", before);
  const lines = Array.isArray(prep.lines) ? prep.lines : null;
  if (!lines || !lines.every((l) => typeof l === "string" && l.startsWith("BENCH PROVISION ")))
    throw new Error("Sunucu yanıtı beklenmedik.");
  for (let i = 0; i < lines.length; i++) {
    onStep("yazılıyor", `${i + 1}/${lines.length}`);
    await con.send(lines[i] as string);
    const r = await con.waitFor((line) => line.match(SETUP)?.[1] ?? null, 60_000);
    if (r !== "SETUP PASS")
      throw new Error(r ? `Tasma kurulumu reddetti: ${r}` : "Tasma 60 saniyede yanıt vermedi.");
  }
  onStep("onaylanıyor");
  const after = await readTrackerReport(con);
  if (after.device !== before.device) throw new Error("Bağlı tasma değişti.");
  const done = await server.devicePost("/api/v1/devices/tracker/confirm", after);
  const gateways = Array.isArray(done.gateways) ? done.gateways.filter((g) => typeof g === "string") as string[] : [];
  return { device: after.device, gateways, added: typeof done.added === "number" ? done.added : 0 };
}

/** 2c: USB ile bağlı cihaz ne? Gateway ise köprü bağlantısı, tasma ise kartı açılır. */
export type UsbDevice = { kind: "gateway"; id: string } | { kind: "tracker"; report: TrackerReport };

export async function probeUsbDevice(con: LineConsole): Promise<UsbDevice> {
  await con.send("BENCH?");
  let cred: RegExpMatchArray | null = null;
  const found = await con.waitFor<{ gateway: string } | { tracker: RegExpMatchArray }>((line) => {
    const g = line.match(GATEWAY);
    if (g) return { gateway: g[1] };
    const c = line.match(CREDENTIAL);
    if (c) cred = c;
    const t = line.match(TRACKER);
    return t && cred ? { tracker: t } : null;
  }, 6000);
  if (!found) throw new Error("Cihaz yanıt vermedi. Kabloyu ve cihaz yazılımını kontrol et.");
  if ("gateway" in found) return { kind: "gateway", id: found.gateway };
  const t = found.tracker, c = cred as unknown as RegExpMatchArray;
  return {
    kind: "tracker",
    report: {
      device: t[1], granted: t[2], credential: c[1],
      epoch: parseInt(c[2], 16), generation: parseInt(c[3], 16), floor: parseInt(c[4], 16),
    },
  };
}
