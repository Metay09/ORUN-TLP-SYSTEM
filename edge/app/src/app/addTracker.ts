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
// Y2a tek yazılım: her BENCH? yanıtı rol satırıyla başlar. Rolü henüz seçilmemiş
// (boş) ya da iki rolün verisini birden taşıyan cihaz yalnız bu satırı verir.
const ROLE_LINE = /BENCH DEVICE id=([0-9A-F]{16}) role=(UNASSIGNED|CONFLICT) security_region=\S+ gnss=(\w+)/;

/** Cihazda GPS takılı mı (rol önerisi için): yes / no / unknown (henüz bakılmadı). */
export type GnssHint = "yes" | "no" | "unknown";
const gnssHint = (s: string): GnssHint => (s === "yes" || s === "no" ? s : "unknown");

/** Kurulumda seçilen rol (sahip kararı 2026-11-03: kurulumda sor, GPS'e göre öner). */
export type DeviceRole = "tasma" | "gateway";
export function suggestedRole(gnss: GnssHint): DeviceRole | null {
  return gnss === "yes" ? "tasma" : gnss === "no" ? "gateway" : null;
}

/** Rolü olmayan cihaz için kullanıcıya gösterilen neden. */
export function noRoleMessage(role: string): string {
  return role === "CONFLICT"
    ? "Bu cihazda hem tasma hem gateway verisi var; cihaza bir şey yazılmadı. Destek gerekiyor."
    : "Bu cihaz henüz kurulmamış: tasma mı gateway mi olacağı seçilmedi.";
}

/** readTrackerReport'un kurulmamış (ya da çelişkili) cihaz için attığı hata. */
export class NoRoleError extends Error {
  readonly id: string;
  readonly gnss: GnssHint;
  readonly conflict: boolean;
  constructor(id: string, gnss: GnssHint, conflict: boolean) {
    super(noRoleMessage(conflict ? "CONFLICT" : "UNASSIGNED"));
    this.id = id; this.gnss = gnss; this.conflict = conflict;
  }
}

/** Ask the device who it is. Gateways are added by their bridge, not here. */
export async function readTrackerReport(con: LineConsole): Promise<TrackerReport> {
  await con.send("BENCH?");
  let cred: RegExpMatchArray | null = null;
  let noRole: RegExpMatchArray | null = null;
  const tracker = await con.waitFor((line) => {
    if (GATEWAY.test(line)) return "gateway" as const;
    const n = line.match(ROLE_LINE);
    if (n) { noRole = n; return "none" as const; }
    const c = line.match(CREDENTIAL);
    if (c) cred = c;
    const t = line.match(TRACKER);
    return t && cred ? t : null;
  }, 6000);
  if (tracker === "gateway")
    throw new Error("Bu bir gateway. Gateway'ler köprüye (PC Edge ya da telefon) bağlanınca çiftliğe kendiliğinden eklenir.");
  if (tracker === "none") {
    const n = noRole as unknown as RegExpMatchArray;
    throw new NoRoleError(n[1], gnssHint(n[3]), n[2] === "CONFLICT");
  }
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

/** 2c: USB ile bağlı cihaz ne? Gateway ise köprü bağlantısı, tasma ise kartı açılır;
 *  kurulmamış cihaza rol sorulur (Y2a). */
export type UsbDevice =
  | { kind: "gateway"; id: string }
  | { kind: "tracker"; report: TrackerReport }
  | { kind: "unassigned"; id: string; gnss: GnssHint }
  | { kind: "conflict"; id: string };

export async function probeUsbDevice(con: LineConsole): Promise<UsbDevice> {
  await con.send("BENCH?");
  let cred: RegExpMatchArray | null = null;
  const found = await con.waitFor<{ gateway: string } | { tracker: RegExpMatchArray } | { noRole: RegExpMatchArray }>((line) => {
    const g = line.match(GATEWAY);
    if (g) return { gateway: g[1] };
    const n = line.match(ROLE_LINE);
    if (n) return { noRole: n };
    const c = line.match(CREDENTIAL);
    if (c) cred = c;
    const t = line.match(TRACKER);
    return t && cred ? { tracker: t } : null;
  }, 6000);
  if (!found) throw new Error("Cihaz yanıt vermedi. Kabloyu ve cihaz yazılımını kontrol et.");
  if ("gateway" in found) return { kind: "gateway", id: found.gateway };
  if ("noRole" in found) {
    const n = found.noRole;
    return n[2] === "CONFLICT" ? { kind: "conflict", id: n[1] } : { kind: "unassigned", id: n[1], gnss: gnssHint(n[3]) };
  }
  const t = found.tracker, c = cred as unknown as RegExpMatchArray;
  return {
    kind: "tracker",
    report: {
      device: t[1], granted: t[2], credential: c[1],
      epoch: parseInt(c[2], 16), generation: parseInt(c[3], 16), floor: parseInt(c[4], 16),
    },
  };
}

// ---- Y2a: boş cihaza rol ver, yeniden başlayınca yeniden bağlan ----

const APP_SET = /^APP SET id=\d+ code=(\w+)(?:.*?\breason=(\S+))?/;

/** Seçimi cihaza yazar (kayıtlı hizmet: tasma = T, gateway = A). Cihaz rolü
 *  kabul edince "ROLE chosen=" der, yanıtını verir ve ~3 sn sonra o rolde
 *  yeniden başlar. Rolü belli bir cihaz seçimi reddeder. */
export async function chooseRole(con: LineConsole, role: DeviceRole): Promise<void> {
  await con.send(role === "tasma" ? "APP SERVICES T" : "APP SERVICES A");
  const r = await con.waitFor<{ ok: true } | { ok: false; code: string; reason: string }>((line) => {
    if (line.startsWith("ROLE chosen=")) return { ok: true };
    if (line.trim() === "APP REJECTED") return { ok: false, code: "REJECTED", reason: "" };
    const m = line.match(APP_SET);
    // "ROLE chosen" yanıttan önce gelir: önce APPLIED geldiyse rol değişmedi.
    if (m) return { ok: false, code: m[1], reason: m[2] ?? "" };
    return null;
  }, 15_000);
  if (!r) throw new Error("Cihaz rol seçimine yanıt vermedi.");
  if (!r.ok) {
    if (r.reason.startsWith("device_is_") || r.code === "APPLIED" || r.code === "UNCHANGED")
      throw new Error("Bu cihazın rolü zaten belli; değiştirmek için cihazın sıfırlanması gerekir.");
    throw new Error(`Cihaz rolü kaydetmedi (${r.code}).`);
  }
}

/** Cihaz yeniden başlarken USB kopar; birkaç kez yeniden açmayı dener.
 *  İzin reddedilirse beklemeden durur. */
export async function reopenAfterRestart<C>(
  open: () => Promise<C>,
  sleep: (ms: number) => Promise<void> = (ms) => new Promise((r) => setTimeout(r, ms)),
  { firstWaitMs = 5000, gapMs = 2000, tries = 12 } = {},
): Promise<C> {
  await sleep(firstWaitMs);
  for (let i = 0; i < tries; i++) {
    try { return await open(); } catch (e) {
      if (e instanceof Error && /izni verilmedi/.test(e.message)) throw e;
    }
    if (i + 1 < tries) await sleep(gapMs);
  }
  throw new Error("Cihaz yeniden başladıktan sonra USB açılamadı. Kabloyu çıkarıp tak, tekrar dene.");
}

export interface ClosableConsole extends LineConsole { close(): Promise<void> }

/** Rolü yazar, cihaz yeniden başlayınca yeniden bağlanır ve seçilen rolde
 *  açıldığını doğrular. Dönen bağlantıyı çağıran kapatır. */
export async function setRoleAndReconnect<C extends ClosableConsole>(
  con: C, role: DeviceRole, open: () => Promise<C>,
  sleep?: (ms: number) => Promise<void>,
): Promise<{ con: C; device: UsbDevice }> {
  await chooseRole(con, role);
  await con.close();
  const next = await reopenAfterRestart(open, sleep);
  try {
    const device = await probeUsbDevice(next);
    if (device.kind !== (role === "tasma" ? "tracker" : "gateway"))
      throw new Error("Cihaz seçilen rolle açılmadı; bu durumu Claude'a bildir.");
    return { con: next, device };
  } catch (e) {
    await next.close();
    throw e;
  }
}
