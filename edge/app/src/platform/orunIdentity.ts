// Step3C native Android identity preflight. This is NOT server enrollment.
// No private key material, bearer token or Tracker read key is exposed here.
import { Capacitor, registerPlugin } from "@capacitor/core";

export interface AndroidIdentity {
  present: boolean;
  installationId?: string;
  fingerprint?: string;
  publicKeyBase64?: string;
}

interface NativeIdentity {
  getIdentity(options: { create: boolean }): Promise<AndroidIdentity>;
  signEnrollment(options: {
    requestId: string;
    challengeBase64: string;
  }): Promise<{ signatureBase64: string; installationId: string }>;
}

const native = registerPlugin<NativeIdentity>("OrunIdentity");
const ID = /^edge-[0-9a-f]{64}$/;
const FP = /^[0-9A-F]{20}$/;
const B64 = /^[A-Za-z0-9+/]{43}=$/;

export const isAndroidIdentityAvailable = (): boolean =>
  Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android";

export function validateIdentity(data: AndroidIdentity): AndroidIdentity {
  if (!data || typeof data !== "object" || typeof data.present !== "boolean") {
    throw new Error("Telefon kimlik yanıtı geçersiz.");
  }
  if (data.present) {
    if (!data.installationId || !ID.test(data.installationId) ||
        !data.fingerprint || !FP.test(data.fingerprint) ||
        !data.publicKeyBase64 || !B64.test(data.publicKeyBase64)) {
      throw new Error("Telefon kimliği doğrulanamadı.");
    }
  }
  return data;
}

/** Client-side-safe fault classification: never display raw native error text,
 * provider diagnostics, installation ID or potential credentials. */
export type IdentityFault =
  | "BRIDGE_UNAVAILABLE" | "READ_FAILED" | "CREATE_FAILED"
  | "PUBLIC_KEY_FAILED" | "SIGN_VERIFY_FAILED" | "IDENTITY_FORMAT_FAILED"
  | "RESPONSE_INVALID" | "UNKNOWN";

const NATIVE_STAGES: Readonly<Record<string, IdentityFault>> = {
  ORUN_IDENTITY_READ_FAILED: "READ_FAILED",
  ORUN_IDENTITY_CREATE_FAILED: "CREATE_FAILED",
  ORUN_IDENTITY_PUBLIC_KEY_FAILED: "PUBLIC_KEY_FAILED",
  ORUN_IDENTITY_SIGN_VERIFY_FAILED: "SIGN_VERIFY_FAILED",
  ORUN_IDENTITY_IDENTITY_FORMAT_FAILED: "IDENTITY_FORMAT_FAILED",
};

export function identityFault(error: unknown): IdentityFault {
  if (!error || typeof error !== "object") return "UNKNOWN";
  const item = error as { code?: unknown; message?: unknown };
  for (const value of [item.code, item.message]) {
    if (typeof value !== "string") continue;
    if (Object.hasOwn(NATIVE_STAGES, value)) return NATIVE_STAGES[value];
    if (/is not implemented on android|plugin is not implemented|no implementation found/i.test(value))
      return "BRIDGE_UNAVAILABLE";
    if (value === "Telefon kimlik yanıtı geçersiz." ||
        value === "Telefon kimliği doğrulanamadı.") return "RESPONSE_INVALID";
  }
  return "UNKNOWN";
}

/** create:true must follow an explicit user press; never call on boot. */
export async function inspectIdentity(create = false): Promise<AndroidIdentity> {
  if (!isAndroidIdentityAvailable()) throw new Error("Bu özellik Android cihaz içindir.");
  return validateIdentity(await native.getIdentity({ create }));
}

/** Later Step3C enrollment UI only. The backend challenge must be obtained
 * from an authenticated HTTPS service; no arbitrary-message signing API. */
export async function signIdentityEnrollment(requestId: string,
                                             challengeBase64: string):
  Promise<{ signatureBase64: string; installationId: string }> {
  if (!isAndroidIdentityAvailable() || !/^[0-9a-f]{32}$/.test(requestId) ||
      !B64.test(challengeBase64)) throw new Error("Kayıt isteği geçersiz.");
  const result = await native.signEnrollment({requestId,challengeBase64});
  if (!result || !ID.test(result.installationId) ||
      typeof result.signatureBase64 !== "string" ||
      !/^[A-Za-z0-9+/]{86}==$/.test(result.signatureBase64)) {
    throw new Error("Cihaz imzası biçimi geçersiz.");
  }
  return result;
}
