// 4c: read-only baseline and post-update evidence for an ORUN RAK4630.
// Mirrors tools/orun_cihaz_kontrol.py. Never captures an actual secret key.
import type { LineConsole } from "./addTracker.ts";

export type DeviceKind = "tracker" | "gateway";
export interface FirmwareEvidence {
  kind: DeviceKind;
  id: string;
  firmware: string | null;
  credential?: string;
  epoch?: string;
  granted?: string;
  grantGeneration?: string;
  testCredential?: boolean;
  storeInc?: string;
  nextSeq?: number;
  storeReady?: boolean;
  storeFaulted?: boolean;
  custodyRegion?: string;
  held?: number;
  ogk1Generation?: string;
}

/** Read-only lines: BENCH?, PRODUCT STORE?, APP DEVICE?. */
export function parseFirmwareEvidence(lines: string[]): FirmwareEvidence {
  const out: Partial<FirmwareEvidence> = {};
  for (const line of lines) {
    let m: RegExpMatchArray | null;
    if ((m = line.match(/^BENCH DEVICE id=([0-9A-F]{16}) role=(TRACKER|GATEWAY)\b/))) {
      out.id = m[1]; out.kind = m[2] === "TRACKER" ? "tracker" : "gateway";
    } else if ((m = line.match(/^BENCH TRACKER id=([0-9A-F]{16}) .*?granted_gateway=([0-9A-F]{16}|none)\b/))) {
      if (out.id && out.id !== m[1]) throw new Error("Cihaz kimlikleri çelişkili.");
      if (out.kind && out.kind !== "tracker") throw new Error("Cihaz türleri çelişkili.");
      out.id = m[1]; out.kind = "tracker"; out.granted = m[2];
      const testKey = line.match(/\btest_credential=(yes|no)\b/);
      if (testKey) out.testCredential = testKey[1] === "yes";
    } else if ((m = line.match(/^BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) grant_generation=([0-9A-F]{8})/))) {
      out.credential = m[1]; out.epoch = m[2]; out.grantGeneration = m[3];
    } else if ((m = line.match(/^BENCH GATEWAY id=([0-9A-F]{16}) custody_region=(\w+)(?: custody_ready=\w+ held=(-?\d+)\/\d+)?/))) {
      if (out.id && out.id !== m[1]) throw new Error("Cihaz kimlikleri çelişkili.");
      if (out.kind && out.kind !== "gateway") throw new Error("Cihaz türleri çelişkili.");
      out.id = m[1]; out.kind = "gateway"; out.custodyRegion = m[2];
      if (m[3]) out.held = Number(m[3]);
    } else if ((m = line.match(/^BENCH OGK1 status=\w+ generation=([0-9A-F]{16})/))) {
      out.ogk1Generation = m[1];
    } else if ((m = line.match(/^PRODUCT STORE ready=(yes|no) faulted=(yes|no) inc=(\w+) next_seq=(\d+)/))) {
      out.storeReady = m[1] === "yes"; out.storeFaulted = m[2] === "yes";
      out.storeInc = m[3]; out.nextSeq = Number(m[4]);
    } else if ((m = line.match(/^APP DEVICE id=\d+ fw=(\S+)/))) {
      out.firmware = m[1];
    }
  }
  if (!out.id || !out.kind || !out.firmware) throw new Error("Cihazın kimliği, türü veya yazılımı okunamadı.");
  if (out.kind === "tracker" && out.testCredential === true)
    throw new Error("Bu tasma deneme anahtarı kullanıyor; yükleme ön denetimi reddedildi.");
  if (out.kind === "tracker" && (!out.credential || out.credential === "none" || !out.epoch ||
      !out.granted || !out.grantGeneration || !out.storeInc || out.nextSeq === undefined ||
      out.nextSeq === 0 || !out.storeReady || out.storeFaulted))
    throw new Error("Tasmanın anahtar ve kayıt hafızası güvenilir biçimde okunamadı.");
  if (out.kind === "gateway" && (out.custodyRegion !== "READY" || out.held === undefined ||
      out.held < 0 || !out.ogk1Generation))
    throw new Error("Gateway belleği, bekleyen kayıt sayısı veya imzalı izin okunamadı.");
  return out as FirmwareEvidence;
}

export async function readFirmwareEvidence(con: LineConsole): Promise<FirmwareEvidence> {
  const lines: string[] = [];
  for (const command of ["BENCH?", "PRODUCT STORE?", "APP DEVICE?"]) {
    await con.send(command);
    // Wait for a short collection window; consumes only what firmware answered.
    await con.waitFor((line) => { lines.push(line); return null; }, 1100);
  }
  return parseFirmwareEvidence(lines);
}

export interface EvidenceCheck { ok: boolean; reasons: string[]; }
/** No "successful update" without all of these post-reset invariants. */
export function compareFirmwareEvidence(before: FirmwareEvidence, after: FirmwareEvidence, sourceCommit: string): EvidenceCheck {
  const problems: string[] = [];
  const need = (ok: boolean, reason: string) => { if (!ok) problems.push(reason); };
  need(before.id === after.id, "Cihaz kimliği değişti.");
  need(before.kind === after.kind, "Cihaz türü değişti.");
  const expectedBuild = sourceCommit.slice(0, 7);
  need(after.firmware !== null && new RegExp("(?:^|-)" + expectedBuild + "(?:\\+)?$").test(after.firmware),
    "Yeni yazılım sürümü doğrulanamadı.");
  if (before.kind === "tracker") {
    need(before.credential === after.credential && before.epoch === after.epoch, "Anahtar kimliği veya dönemi değişti.");
    need(before.granted === after.granted && before.grantGeneration === after.grantGeneration, "Gateway izni değişti.");
    need(before.storeInc === after.storeInc, "Kayıt hafızası kimliği değişti.");
    need(after.storeReady === true && after.storeFaulted === false, "Kayıt hafızası sağlıklı değil.");
    need(after.nextSeq !== undefined && before.nextSeq !== undefined && after.nextSeq >= before.nextSeq,
      "Kayıt sayacı geri gitti veya okunamadı.");
  } else {
    need(after.custodyRegion === "READY",
      "Gateway hafızası hazır değil.");
    if (before.held !== undefined) need(after.held !== undefined && after.held >= before.held,
      "Gateway'in bekleyen kayıt sayısı azaldı.");
    if (before.ogk1Generation) need(after.ogk1Generation === before.ogk1Generation, "Gateway imzalı izni değişti.");
  }
  return { ok: problems.length === 0, reasons: problems };
}
