// ORUN Step 3E: owner-initiated AndroidKeyStore P-256 identity.
// This module NEVER stores or returns an exportable private key.
// The existence of this plugin is not evidence of server enrollment.
import { Capacitor, registerPlugin } from "@capacitor/core";

export interface OrunMobilePublicIdentity {
  present: boolean;
  algorithm?: "P256-SHA256";
  installationId?: string;
  fingerprint?: string;
  publicKeyBase64?: string;
}
export interface OrunEnrollmentSignature {
  installationId: string;
  signatureBase64: string;
}
interface NativeIdentity {
  getIdentity(options: { create: boolean }): Promise<OrunMobilePublicIdentity>;
  signEnrollment(options: {
    requestId: string;
    challengeBase64: string;
  }): Promise<OrunEnrollmentSignature>;
}
const native = registerPlugin<NativeIdentity>("OrunMobileIdentity");
const INSTALLATION = /^edge-[0-9a-f]{64}$/;
const FP = /^[0-9A-F]{20}$/;
const REQUEST_ID = /^[0-9a-f]{32}$/;
const CHALLENGE = /^[A-Za-z0-9+/]{43}=$/;
const BASE64 = /^[A-Za-z0-9+/]+={0,2}$/;

export function isMobileIdentityAvailable(): boolean {
  return Capacitor.isNativePlatform() && Capacitor.getPlatform() === "android";
}

/** create=true is authorized ONLY following an explicit user press. */
export async function mobileIdentity(create = false):
  Promise<OrunMobilePublicIdentity> {
  if (!isMobileIdentityAvailable())
    throw new Error("Güvenli telefon kaydı Android uygulamasında kullanılabilir.");
  const value = await native.getIdentity({ create });
  if (!value || typeof value.present !== "boolean")
    throw new Error("Telefon kimlik yanıtı geçersiz.");
  if (value.present && (value.algorithm !== "P256-SHA256" ||
      !value.installationId || !INSTALLATION.test(value.installationId) ||
      !value.fingerprint || !FP.test(value.fingerprint) ||
      !value.publicKeyBase64 || !BASE64.test(value.publicKeyBase64) ||
      value.publicKeyBase64.length > 256)) {
    throw new Error("Telefonun güvenli kimliği doğrulanamadı.");
  }
  return value;
}

/** No arbitrary signing. Signs ONLY the server registration challenge. */
export async function signMobileEnrollment(requestId: string,
                                           challengeBase64: string):
  Promise<OrunEnrollmentSignature> {
  if (!isMobileIdentityAvailable() || !REQUEST_ID.test(requestId) ||
      !CHALLENGE.test(challengeBase64))
    throw new Error("Telefon kayıt isteği geçersiz.");
  const answer = await native.signEnrollment({ requestId, challengeBase64 });
  if (!answer || !INSTALLATION.test(answer.installationId) ||
      !BASE64.test(answer.signatureBase64) ||
      answer.signatureBase64.length > 120) {
    throw new Error("Telefon kayıt imzası geçersiz.");
  }
  return answer;
}
