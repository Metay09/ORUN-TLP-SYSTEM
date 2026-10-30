// ORUN S5B AndroidKeyStore-encrypted phone session, stored in noBackupFilesDir.
// No browser localStorage/sessionStorage/IndexedDB token persistence.
// A protected native file is not a substitute for correct login authorization.
import { Capacitor, registerPlugin } from "@capacitor/core";

export interface MobileSessionMeta {
  present: boolean;
  origin?: string;
  installation?: string;
  site?: string;
  expiresAt?: number;
}
export interface ActiveMobileSession {
  present: true;
  origin: string;
  installation: string;
  site: string;
  expiresAt: number;
  accessToken: string;
}
export interface MobileSessionInput {
  origin: string;
  installation: string;
  site: string;
  expiresAt: number;
  accessToken: string;
}
interface NativeSession {
  putSession(value: MobileSessionInput): Promise<{ stored: boolean }>;
  getSession(value: { includeToken: boolean }): Promise<MobileSessionMeta & {
    accessToken?: string;
  }>;
  clearSession(): Promise<{ cleared: boolean }>;
}

const plugin = registerPlugin<NativeSession>("OrunMobileSession");
const INST = /^edge-[a-f0-9]{64}$/;
const SITE = /^[A-Za-z0-9._-]{1,128}$/;
const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;
const MAX_SECONDS = 30 * 24 * 3600 + 600;

function requireAndroid(): void {
  if (!Capacitor.isNativePlatform() || Capacitor.getPlatform() !== "android")
    throw new Error("Güvenli oturum saklama yalnız Android ORUN uygulamasında kullanılabilir.");
}

function validOrigin(raw: string): boolean {
  try {
    const parsed = new URL(raw);
    return parsed.protocol === "https:" && !parsed.username &&
      !parsed.password && parsed.pathname === "/" && !parsed.search &&
      !parsed.hash && parsed.origin === raw;
  } catch {
    return false;
  }
}

function validMeta(value: MobileSessionMeta): boolean {
  return !!value && value.present === true &&
    typeof value.origin === "string" && validOrigin(value.origin) &&
    typeof value.installation === "string" && INST.test(value.installation) &&
    typeof value.site === "string" && SITE.test(value.site) &&
    typeof value.expiresAt === "number" &&
    Number.isSafeInteger(value.expiresAt) && value.expiresAt > Date.now() / 1000;
}

export async function storeMobileSession(value: MobileSessionInput): Promise<void> {
  requireAndroid();
  if (!validMeta({ ...value, present: true }) ||
      !TOKEN.test(value.accessToken) ||
      value.expiresAt > Date.now() / 1000 + MAX_SECONDS)
    throw new Error("Telefon yetkisi geçersiz veya süresi dolmuş.");
  const saved = await plugin.putSession(value);
  if (!saved?.stored) throw new Error("Telefon yetkisi güvenli saklanamadı.");
}

/** Default read intentionally does not bring the bearer into JS. */
export async function mobileSessionStatus(): Promise<MobileSessionMeta> {
  requireAndroid();
  const answer = await plugin.getSession({ includeToken: false });
  if (!answer?.present) return { present: false };
  if (!validMeta(answer) || answer.accessToken !== undefined)
    throw new Error("Korunan telefon oturumu geçersiz.");
  return answer;
}

/** Use only for authenticated API requests; never log or persist the result. */
export async function activeMobileSession(): Promise<ActiveMobileSession | null> {
  requireAndroid();
  const answer = await plugin.getSession({ includeToken: true });
  if (!answer?.present) return null;
  if (!validMeta(answer) || typeof answer.accessToken !== "string" ||
      !TOKEN.test(answer.accessToken))
    throw new Error("Korunan telefon oturumu geçersiz.");
  return answer as ActiveMobileSession;
}

/** Local sign-out. This does NOT revoke the server token or phone identity. */
export async function clearLocalMobileSession(): Promise<void> {
  requireAndroid();
  const answer = await plugin.clearSession();
  if (!answer?.cleared) throw new Error("Telefon oturumu temizlenemedi.");
}
