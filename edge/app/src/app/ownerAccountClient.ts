// ORUN account-login transport. Password-only sign-in is the primary path.
// The approved phone's device bearer comes from native Keystore-backed
// storage; short-lived account bearer remains ONLY in this JS class instance.
// No localStorage, IndexedDB, URL, logs, or observation-bench token reuse.
import type { ActiveMobileSession } from "../platform/orunMobileSession.ts";
import {
  parseServerLatest, parseServerObservationPage,
  type ServerLatestObservations, type ServerObservationPage,
} from "./serverObservationRead.ts";

export interface AccountIdentity {
  site: string;
  actor: string;
  username: string;
  installation: string;
  expiresAt: number;
}

const NAME = /^[a-z][a-z0-9._-]{2,63}$/;
const EMAIL = /^[a-z0-9._%+-]{1,64}@[a-z0-9.-]{1,190}\.[a-z]{2,24}$/;
// 1b: server sessions last 180 days and slide forward on use.
const MAX_SESSION_SECONDS = 200 * 24 * 3600;
const TOKEN = /^[A-Za-z0-9_-]{48,128}$/;
const IDENT = /^edge-[0-9a-f]{64}$/;
const ACCOUNT_INST = /^account-[0-9a-f]{64}$/;
const ID = /^[A-Za-z0-9._-]{1,128}$/;
const MAX_BYTES = 8192;
const MAX_SYNC_BYTES = 512 * 1024; // shared sync backend page, not login credential response

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

async function json(reply: Response, limit = MAX_BYTES): Promise<Record<string, unknown>> {
  if (!reply.headers.get("content-type")?.includes("application/json"))
    throw new Error("Sunucudan beklenmeyen yanıt.");
  const raw = await reply.text();
  if (raw.length > limit)
    throw new Error("Sunucu yanıtı çok büyük.");
  const parsed: unknown = JSON.parse(raw);
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed))
    throw new Error("Sunucu yanıtı hatalı.");
  return parsed as Record<string, unknown>;
}

export class OwnerAccountClient {
  private accessToken: string | null = null;
  private identity: AccountIdentity | null = null;
  private epoch = 0;
  get sessionEpoch(): number { return this.epoch; }
  private readonly base: string;
  private readonly fetcher: typeof fetch;

  constructor(baseUrl: string, fetcher: typeof fetch = fetch) {
    this.base = origin(baseUrl);
    this.fetcher = fetcher;
  }

  get current(): AccountIdentity | null {
    return this.identity ? { ...this.identity } : null;
  }

  /** Internal upload credential for the authenticated ORUN HTTPS relay.
   * The legacy observation Backend bearer never reaches the phone.
   * Never persist this bearer or display it in a component/log.
   */
  observationTransport(): { url: string; token: string } | null {
    if (!this.accessToken || !this.identity ||
        this.identity.expiresAt <= Date.now() / 1000) return null;
    return { url: this.base, token: this.accessToken };
  }

  /** Main user flow: any phone, username and password. No P256 enrollment.
   * TLS required; account bearer stays only in memory, never in localStorage.
   * A stable server-derived account cache namespace is NOT a phone identity.
   */
  async signInWithPassword(login: string, password: string): Promise<AccountIdentity> {
    const loginEpoch = ++this.epoch;
    this.accessToken = null;
    this.identity = null;
    // Kullanıcı adı ya da e-posta.
    const username = login.includes("@") ? login.trim().toLowerCase() : login.trim();
    if (!(NAME.test(username) || EMAIL.test(username)) || typeof password !== "string" ||
        password.length < 12 || password.length > 128)
      throw new Error("Kullanıcı adı veya parola geçersiz.");
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15_000);
    try {
      const result = await this.fetcher(this.base + "/api/v1/account/login", {
        method: "POST", credentials: "omit", redirect: "error",
        cache: "no-store", signal: controller.signal,
        headers: { "Content-Type": "application/json", Accept: "application/json" },
        body: JSON.stringify({ username, password }),
      });
      if (result.status === 429)
        throw new Error("Çok fazla başarısız giriş. Bir süre sonra yeniden dene.");
      if (result.status === 401)
        throw new Error("Kullanıcı adı veya parola yanlış.");
      if (!result.ok)
        throw new Error("Hesap sunucusu girişi tamamlayamadı.");
      const body = await json(result);
      if (loginEpoch !== this.epoch)
        throw new Error("Giriş sırasında hesap oturumu değişti.");
      const now = Date.now() / 1000;
      if (typeof body.site !== "string" || !ID.test(body.site) ||
          typeof body.actor !== "string" || !ID.test(body.actor) ||
          typeof body.username !== "string" || !NAME.test(body.username) ||
          (!username.includes("@") && body.username !== username) ||
          typeof body.installation !== "string" ||
          !ACCOUNT_INST.test(body.installation) ||
          typeof body.account_access_token !== "string" ||
          !TOKEN.test(body.account_access_token) ||
          typeof body.expires_at !== "number" ||
          !Number.isSafeInteger(body.expires_at) ||
          body.expires_at <= now || body.expires_at > now + MAX_SESSION_SECONDS)
        throw new Error("Sunucudan gelen hesap bilgileri doğrulanamadı.");
      this.accessToken = body.account_access_token;
      this.identity = {
        site: body.site, actor: body.actor, username: body.username,
        installation: body.installation, expiresAt: body.expires_at,
      };
      return { ...this.identity };
    } finally {
      clearTimeout(timeout);
    }
  }

  /** 1b: bring back a session stored on this phone (no network). */
  adopt(identity: AccountIdentity, accessToken: string): void {
    if (!TOKEN.test(accessToken) || !ID.test(identity.site) || !ID.test(identity.actor) ||
        !ACCOUNT_INST.test(identity.installation) || !NAME.test(identity.username))
      throw new Error("Saklanan oturum geçersiz.");
    this.epoch++;
    this.accessToken = accessToken;
    this.identity = { ...identity };
  }

  /** The stored bearer (only for saving it in the phone's protected store). */
  storableSession(): { identity: AccountIdentity; accessToken: string } | null {
    return this.accessToken && this.identity
      ? { identity: { ...this.identity }, accessToken: this.accessToken } : null;
  }

  /** Ask the server whether the session is still valid; refreshes expiry and
   * e-mail. Returns "ok", "expired" (server said no) or "offline". */
  async refresh(): Promise<{ state: "ok" | "expired" | "offline"; email: string | null }> {
    const token = this.accessToken;
    if (!token || !this.identity) return { state: "expired", email: null };
    const epoch = this.epoch;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15_000);
    try {
      const reply = await this.fetcher(this.base + "/api/v1/account/me", {
        headers: { Authorization: "Bearer " + token, Accept: "application/json" },
        credentials: "omit", redirect: "error", cache: "no-store", signal: controller.signal,
      });
      if (epoch !== this.epoch) return { state: "offline", email: null };
      if (reply.status === 401 || reply.status === 403) {
        this.accessToken = null;
        this.identity = null;
        return { state: "expired", email: null };
      }
      if (!reply.ok) return { state: "offline", email: null };
      const body = await json(reply);
      const now = Date.now() / 1000;
      if (typeof body.expires_at === "number" && Number.isSafeInteger(body.expires_at) &&
          body.expires_at > now && body.expires_at <= now + MAX_SESSION_SECONDS && this.identity)
        this.identity = { ...this.identity, expiresAt: body.expires_at };
      return { state: "ok", email: typeof body.email === "string" ? body.email : null };
    } catch {
      return { state: "offline", email: null };
    } finally {
      clearTimeout(timeout);
    }
  }

  /** Optional login e-mail for this account. */
  async setEmail(email: string): Promise<string> {
    const value = email.trim().toLowerCase();
    if (!EMAIL.test(value)) throw new Error("E-posta adresi geçersiz.");
    if (!this.accessToken) throw new Error("Önce hesabınla giriş yapmalısın.");
    const reply = await this.fetcher(this.base + "/api/v1/account/email", {
      method: "POST", credentials: "omit", redirect: "error", cache: "no-store",
      headers: { Authorization: "Bearer " + this.accessToken,
                 "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify({ email: value }),
    });
    if (reply.status === 409) throw new Error("Bu e-posta başka bir hesapta kullanılıyor.");
    if (!reply.ok) throw new Error("E-posta kaydedilemedi.");
    const body = await json(reply);
    if (body.email !== value) throw new Error("Sunucu yanıtı beklenmedik.");
    return value;
  }

  /** 1d/1e: POST to an owner-only device setup route. */
  async devicePost(path: "/api/v1/devices/tracker/prepare" | "/api/v1/devices/tracker/confirm",
                   body: unknown): Promise<Record<string, unknown>> {
    if (!this.accessToken) throw new Error("Önce hesabınla giriş yapmalısın.");
    const reply = await this.fetcher(this.base + path, {
      method: "POST", credentials: "omit", redirect: "error", cache: "no-store",
      headers: { Authorization: "Bearer " + this.accessToken,
                 "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify(body),
    });
    const parsed = await json(reply).catch(() => ({} as Record<string, unknown>));
    if (reply.status === 401) throw new Error("Oturumun geçersiz; yeniden giriş yap.");
    if (!reply.ok)
      throw new Error(typeof parsed.message === "string" && parsed.message.length < 200
        ? parsed.message : "Sunucu cihaz eklemeyi tamamlayamadı.");
    return parsed;
  }

  /** Called only after an explicit user login action. */
  async signIn(device: ActiveMobileSession, username: string,
               password: string): Promise<AccountIdentity> {
    const loginEpoch = ++this.epoch;
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
      if (loginEpoch !== this.epoch)
        throw new Error("Giriş sırasında hesap oturumu değişti.");
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
    const requestEpoch = this.epoch;
    const token = this.accessToken;
    const reply = await this.fetcher(
      this.base + "/api/v1/sync?cursor=" + cursor, {
        headers: { Authorization: "Bearer " + token,
                   Accept: "application/json" },
        credentials: "omit", redirect: "error", cache: "no-store",
      });
    if (requestEpoch !== this.epoch)
      throw new Error("İndirme sırasında hesap oturumu değişti.");
    if (reply.status === 401 || reply.status === 403) {
      this.epoch++;
      this.accessToken = null;
      this.identity = null;
      throw new Error("Hesap yetkisi sona erdi veya iptal edildi.");
    }
    if (!reply.ok) throw new Error("Ortak veriler şu anda alınamadı.");
    const body = await json(reply, MAX_SYNC_BYTES);
    if (requestEpoch !== this.epoch)
      throw new Error("İndirme sırasında hesap oturumu değişti.");
    if (!Array.isArray(body.changes) ||
        typeof body.cursor !== "number" ||
        !Number.isSafeInteger(body.cursor))
      throw new Error("Sunucudan geçersiz ortak veri yanıtı.");
    return body;
  }

  /** Fetch sealed observations from this account's pinned one-site server.
   * Transport-only: caller must AEAD-verify and commit to a separate
   * site-scoped SQLite mirror before displaying anything.
   */
  async readServerObservations(after = 0): Promise<ServerObservationPage> {
    if (!Number.isSafeInteger(after) || after < 0)
      throw new Error("Sunucu kayıt imleci geçersiz.");
    const parsed = await this.readRecords("after=" + after);
    return parseServerObservationPage(parsed, after);
  }

  /** Newest measured position of every tracker (same exact protected frames). */
  async readServerLatest(): Promise<ServerLatestObservations> {
    return parseServerLatest(await this.readRecords("latest=1"));
  }

  private async readRecords(query: string): Promise<unknown> {
    if (!this.accessToken || !this.identity ||
        this.identity.expiresAt <= Date.now() / 1000)
      throw new Error("Önce hesabınla giriş yapmalısın.");
    const epoch = this.epoch;
    const token = this.accessToken;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 20_000);
    try {
      const reply = await this.fetcher(
        this.base + "/v1/edge/records?" + query, {
          headers: { Authorization: "Bearer " + token,
                     Accept: "application/json" },
          credentials: "omit", redirect: "error", cache: "no-store",
          signal: controller.signal,
        });
      if (this.epoch !== epoch)
        throw new Error("Kayıt indirme sırasında hesap değişti.");
      if (reply.status === 401 || reply.status === 403)
        throw new Error("Kayıtları görme yetkisi bulunamadı.");
      if (!reply.ok)
        throw new Error("Sunucudaki tasma kayıtları henüz alınamadı.");
      const parsed = await json(reply, 128 * 1024);
      if (this.epoch !== epoch || !this.accessToken || !this.identity ||
          this.identity.expiresAt <= Date.now() / 1000)
        throw new Error("Kayıt indirme sırasında hesap oturumu sona erdi.");
      return parsed;
    } finally {
      clearTimeout(timeout);
    }
  }

  /** Best-effort server logout, with immediate local memory clearing. */
  async signOut(): Promise<void> {
    this.epoch++;
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
