// ORUN S6A account-login transport candidate. Not wired into live APK.
// The approved phone's device bearer comes from native Keystore-backed
// storage; short-lived account bearer remains ONLY in this JS class instance.
// No localStorage, IndexedDB, URL, logs, or observation-bench token reuse.
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";

export interface AccountIdentity {
  site: string;
  actor: string;
  username: string;
  installation: string;
  expiresAt: number;
}

const NAME = /^[a-z][a-z0-9._-]{2,63}$/;
const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;
const IDENT = /^edge-[0-9a-f]{64}$/;
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const MAX_BYTES = 8192;

function origin(raw: string): string {
  let uri: URL;
  try { uri = new URL(raw); }
  catch { throw new Error("Hesap sunucusunun HTTPS adresi geçersiz."); }
  if (uri.protocol !== "https:" || !uri.hostname || uri.username ||
      uri.password || uri.pathname !== "/" || uri.search || uri.hash ||
      uri.origin !== raw)
    throw new Error("Hesap girişi için güvenli HTTPS adresi gerekli.");
  return uri.origin;
}

async function json(reply: Response): Promise<Record<string, unknown>> {
  if (!reply.headers.get("content-type")?.includes("application/json"))
    throw new Error("Sunucudan beklenmeyen yanıt.");
  const raw = await reply.text();
  if (raw.length > MAX_BYTES)
    throw new Error("Sunucu yanıtı çok büyük.");
  const parsed: unknown = JSON.parse(raw);
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
    throw new Error("Sunucu yanıtı hatalı.");
  return parsed as Record<string, unknown>;
}

export class OwnerAccountClient {
  private accessToken: string | null = null;
  private identity: AccountIdentity | null = null;
  private readonly base: string;
  private readonly fetcher: typeof fetch;

  constructor(baseUrl: string, fetcher: typeof fetch = fetch) {
    this.base = origin(baseUrl);
    this.fetcher = fetcher;
  }

  get current(): AccountIdentity | null {
    return this.identity ? { ...this.identity } : null;
  }

  /** Called only after an explicit user login action. */
  async signIn(device: ActiveMobileSession, username: string,
               password: string): Promise<AccountIdentity> {
    this.accessToken = null;
    this.identity = null;
    if (!device?.present || origin(device.origin) !== this.base ||
        !IDENT.test(device.installation) || !ID.test(device.site) ||
        !TOKEN.test(device.accessToken))
      throw new Error("Önce bu telefonu ORUN'a güvenli şekilde kaydetmelisin.");
    if (!NAME.test(username) || typeof password !== "string" ||
        password.length < 12 || password.length > 128)
      throw new Error("Kullanıcı adı veya parola geçersiz.");
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15_000);
    try {
      const result = await this.fetcher(this.base + "/api/v1/account/login", {
        method: "POST", credentials: "omit", redirect: "error",
        cache: "no-store", signal: controller.signal,
        headers: { "Content-Type": "application/json",
                   Accept: "application/json",
                   Authorization: "Bearer " + device.accessToken },
        body: JSON.stringify({ username, password }),
      });
      if (result.status === 429)
        throw new Error("Çok fazla başarısız giriş. Bir süre sonra yeniden dene.");
      if (result.status === 401)
        throw new Error("Kullanıcı adı, parola veya telefon yetkisi geçersiz.");
      if (!result.ok)
        throw new Error("Hesap sunucusu girişi tamamlayamadı.");
      const body = await json(result);
      const now = Date.now() / 1000;
      if (body.site !== device.site ||
          body.installation !== device.installation ||
          body.username !== username ||
          typeof body.actor !== "string" || !ID.test(body.actor) ||
          typeof body.account_access_token !== "string" ||
          !TOKEN.test(body.account_access_token) ||
          typeof body.expires_at !== "number" ||
          !Number.isSafeInteger(body.expires_at) ||
          body.expires_at <= now || body.expires_at > now + 12 * 3600 + 600)
        throw new Error("Sunucu hesap kimliği bu telefonla uyuşmuyor.");
      this.accessToken = body.account_access_token;
      this.identity = {
        site: device.site, actor: body.actor, username,
        installation: device.installation, expiresAt: body.expires_at,
      };
      return { ...this.identity };
    } finally { clearTimeout(timeout); }
  }

  /** Returns a site-scoped, authenticated response; no offline claim. */
  async readSharedChanges(cursor = 0): Promise<Record<string, unknown>> {
    if (!this.accessToken || !this.identity ||
        this.identity.expiresAt <= Date.now() / 1000)
      throw new Error("Önce hesabınla giriş yapmalısın.");
    if (!Number.isSafeInteger(cursor) || cursor < 0)
      throw new Error("Eşitleme sıra numarası geçersiz.");
    const reply = await this.fetcher(
      this.base + "/api/v1/sync?cursor=" + cursor, {
        headers: { Authorization: "Bearer " + this.accessToken,
                   Accept: "application/json" },
        credentials: "omit", redirect: "error", cache: "no-store",
      });
    if (reply.status === 401 || reply.status === 403) {
      this.accessToken = null;
      this.identity = null;
      throw new Error("Hesap yetkisi sona erdi veya iptal edildi.");
    }
    if (!reply.ok) throw new Error("Ortak veriler şu anda alınamadı.");
    const body = await json(reply);
    if (!Array.isArray(body.changes) ||
        typeof body.cursor !== "number" ||
        !Number.isSafeInteger(body.cursor))
      throw new Error("Sunucudan geçersiz ortak veri yanıtı.");
    return body;
  }

  /** Best-effort server logout, with immediate local memory clearing. */
  async signOut(): Promise<void> {
    const bearer = this.accessToken;
    this.accessToken = null;
    this.identity = null;
    if (!bearer) return;
    const response = await this.fetcher(this.base + "/api/v1/account/logout", {
      method: "POST", headers: {
        Authorization: "Bearer " + bearer,
      },
      credentials: "omit", redirect: "error", cache: "no-store",
    });
    // Local secret has already been dropped. Do not present server revoke
    // as confirmed if the endpoint failed or returned an unexpected status.
    if (response.status !== 200 && response.status !== 401)
      throw new Error("Yerel çıkış yapıldı, sunucuda iptal doğrulanamadı.");
  }
}
