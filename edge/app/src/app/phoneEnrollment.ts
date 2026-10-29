// Explicit Android phone enrollment HTTP transport. NOT account login and NOT
// enabled by default in the app. The server's trusted OWNER approves through
// the separate local command. No existing Edge observation token is reused.
export interface PhonePublicIdentity {
  algorithm: "P256-SHA256";
  installationId: string;
  fingerprint: string;
  publicKeyBase64: string;
}
export interface PhoneEnrollmentPending {
  installationId: string;
  fingerprint: string;
  algorithm: "P256-SHA256";
  requestId: string;
  challengeBase64: string;
  expiresAt: number;
}
export interface PhoneEnrollmentCredential {
  installation: string;
  accessToken: string;
  expiresInSeconds: number;
}
type Transport = typeof fetch;
const BASE64 = /^[A-Za-z0-9+/]+={0,2}$/;
const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;
const APPROVAL = /^[A-Za-z0-9_-]{30,80}$/;
const IDENT = /^edge-[a-f0-9]{64}$/;
const FP = /^[0-9A-F]{20}$/;
const REQUEST = /^[a-f0-9]{32}$/;
const CHALLENGE = /^[A-Za-z0-9+/]{43}=$/;
const MAX_RESPONSE = 8192;

function origin(raw: string): string {
  let value: URL;
  try { value = new URL(raw); }
  catch { throw new Error("Sunucu HTTPS adresi geçersiz."); }
  if (value.protocol !== "https:" || !value.hostname || value.username ||
      value.password || value.pathname !== "/" || value.search || value.hash)
    throw new Error("Telefon kaydı için doğrulanmış HTTPS adresi gerekli.");
  return value.origin;
}

async function post(base: string, route: string, body: unknown,
                    fetcher: Transport): Promise<Record<string, unknown>> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 15_000);
  try {
    const reply = await fetcher(base + route, {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      credentials: "omit",
      redirect: "error",
      cache: "no-store",
      signal: controller.signal,
      body: JSON.stringify(body),
    });
    if (reply.status === 403)
      throw new Error("Telefon kaydı henüz sahip tarafından onaylanmadı veya isteğin süresi doldu.");
    if (reply.status === 429)
      throw new Error("Çok fazla kayıt isteği var. Daha sonra tekrar dene.");
    if (!reply.ok || !reply.headers.get("content-type")?.includes("application/json"))
      throw new Error("Sunucu telefon kayıt isteğini kabul etmedi.");
    const raw = await reply.text();
    if (raw.length > MAX_RESPONSE) throw new Error("Sunucu yanıtı çok büyük.");
    const out: unknown = JSON.parse(raw);
    if (!out || typeof out !== "object" || Array.isArray(out))
      throw new Error("Sunucu yanıtı geçersiz.");
    return out as Record<string, unknown>;
  } finally {
    clearTimeout(timeout);
  }
}

function validIdentity(identity: PhonePublicIdentity): void {
  if (!identity || identity.algorithm !== "P256-SHA256" ||
      !IDENT.test(identity.installationId) || !FP.test(identity.fingerprint) ||
      typeof identity.publicKeyBase64 !== "string" ||
      identity.publicKeyBase64.length > 256 || !BASE64.test(identity.publicKeyBase64))
    throw new Error("Telefonun güvenli kimliği geçersiz.");
}

/** Called ONLY after the owner explicitly requests registration. */
export async function beginPhoneEnrollment(
  baseUrl: string, identity: PhonePublicIdentity,
  fetcher: Transport = fetch,
): Promise<PhoneEnrollmentPending> {
  validIdentity(identity);
  const response = await post(origin(baseUrl), "/api/v1/enroll/begin", {
    algorithm: identity.algorithm, public_key_base64: identity.publicKeyBase64,
  }, fetcher);
  if (response.installation_id !== identity.installationId ||
      response.fingerprint !== identity.fingerprint ||
      response.algorithm !== identity.algorithm ||
      typeof response.request_id !== "string" || !REQUEST.test(response.request_id) ||
      typeof response.challenge_base64 !== "string" ||
      !CHALLENGE.test(response.challenge_base64) ||
      typeof response.expires_at !== "number" ||
      !Number.isSafeInteger(response.expires_at) ||
      response.expires_at <= 0)
    throw new Error("Kayıt isteği telefonun güvenli kimliğiyle uyuşmuyor.");
  return {
    installationId: identity.installationId,
    fingerprint: identity.fingerprint,
    algorithm: identity.algorithm,
    requestId: response.request_id,
    challengeBase64: response.challenge_base64,
    expiresAt: response.expires_at,
  };
}

export type SignPhoneChallenge = (requestId: string, challengeBase64: string) =>
  Promise<{ installationId: string; signatureBase64: string }>;

/** Requires local owner approval code. Returns a bearer only to the caller.
 * Caller MUST persist it using a reviewed native protected credential store;
 * it MUST NOT be placed in localStorage, a URL, the journal, or app logs.
 * Not wired into the app until that native storage and UX are available.
 */
export async function finishPhoneEnrollment(
  baseUrl: string, identity: PhonePublicIdentity,
  pending: PhoneEnrollmentPending, ownerCode: string,
  sign: SignPhoneChallenge, fetcher: Transport = fetch,
): Promise<PhoneEnrollmentCredential> {
  validIdentity(identity);
  if (pending.installationId !== identity.installationId ||
      pending.fingerprint !== identity.fingerprint ||
      pending.algorithm !== identity.algorithm ||
      !REQUEST.test(pending.requestId) || !CHALLENGE.test(pending.challengeBase64) ||
      !APPROVAL.test(ownerCode))
    throw new Error("Telefon kayıt isteği veya sahip onayı geçersiz.");
  const signed = await sign(pending.requestId, pending.challengeBase64);
  if (signed.installationId !== identity.installationId ||
      typeof signed.signatureBase64 !== "string" ||
      !BASE64.test(signed.signatureBase64) ||
      signed.signatureBase64.length > 120)
    throw new Error("İmza bu telefonun kimliğiyle uyuşmuyor.");
  const response = await post(origin(baseUrl), "/api/v1/enroll/finish", {
    algorithm: identity.algorithm,
    public_key_base64: identity.publicKeyBase64,
    request_id: pending.requestId,
    signature_base64: signed.signatureBase64,
    owner_code: ownerCode,
  }, fetcher);
  if (response.installation_id !== identity.installationId ||
      typeof response.access_token !== "string" ||
      !TOKEN.test(response.access_token) ||
      typeof response.expires_in_seconds !== "number" ||
      !Number.isSafeInteger(response.expires_in_seconds) ||
      response.expires_in_seconds <= 0)
    throw new Error("Telefonun yetkilendirme yanıtı geçersiz.");
  return {
    installation: identity.installationId,
    accessToken: response.access_token,
    expiresInSeconds: response.expires_in_seconds,
  };
}
