// ORUN S5B: trusted enrollment completion → authenticated identity check →
// protected native store. This is an explicit user-triggered candidate flow.
// Not wired into the currently deployed APK or main application lifecycle.
import {
  finishPhoneEnrollment, type PhonePublicIdentity,
  type PhoneEnrollmentPending, type SignPhoneChallenge,
} from "./phoneEnrollment.ts";
import { storeMobileSession, type MobileSessionInput } from
  "../platform/orunMobileSession.ts";

export interface PhoneRegistrationDone {
  installation: string;
  site: string;
  expiresAt: number;
}
export type SavePhoneSession = (session: MobileSessionInput) => Promise<void>;

const INST = /^edge-[0-9a-f]{64}$/;
const SITE = /^[A-Za-z0-9._-]{1,128}$/;

function httpsOrigin(raw: string): string {
  const url = new URL(raw);
  if (url.protocol !== "https:" || !url.hostname || url.username ||
      url.password || url.pathname !== "/" || url.search || url.hash)
    throw new Error("HTTPS sunucu adresi geçersiz.");
  return url.origin;
}

/** A signed phone proof alone does not assert which site the owner approved.
 * Resolve trusted site and installation from the issued bearer itself before
 * storing any credential, avoiding client-supplied owner/site scopes.
 */
export async function completePhoneEnrollment(
  baseUrl: string, identity: PhonePublicIdentity,
  pending: PhoneEnrollmentPending, ownerCode: string,
  sign: SignPhoneChallenge,
  fetcher: typeof fetch = fetch,
  save: SavePhoneSession = storeMobileSession,
): Promise<PhoneRegistrationDone> {
  const origin = httpsOrigin(baseUrl);
  const issued = await finishPhoneEnrollment(
    origin, identity, pending, ownerCode, sign, fetcher);
  const response = await fetcher(origin + "/api/v1/sync/identity", {
    headers: { Authorization: "Bearer " + issued.accessToken,
               Accept: "application/json" },
    credentials: "omit",
    redirect: "error",
    cache: "no-store",
  });
  if (!response.ok || !response.headers.get("content-type")?.includes("application/json"))
    throw new Error("Sunucu telefon kimliğini doğrulayamadı.");
  const json: unknown = await response.json();
  if (!json || typeof json !== "object" || Array.isArray(json))
    throw new Error("Sunucu kimlik yanıtı geçersiz.");
  const body = json as Record<string, unknown>;
  if (typeof body.installation !== "string" ||
      !INST.test(body.installation) ||
      body.installation !== issued.installation ||
      typeof body.site !== "string" || !SITE.test(body.site))
    throw new Error("Sunucunun onayladığı telefon kimliği uyuşmuyor.");
  const expiresAt = Math.floor(Date.now() / 1000) + issued.expiresInSeconds;
  if (!Number.isSafeInteger(expiresAt))
    throw new Error("Telefon yetkisi süresi geçersiz.");
  // Store in Android native AES-GCM vault. The bearer is intentionally NOT
  // returned to UI, persisted in JS, logged or sent to observation backend.
  await save({
    origin, installation: issued.installation,
    site: body.site, accessToken: issued.accessToken, expiresAt,
  });
  return { installation: issued.installation, site: body.site, expiresAt };
}
