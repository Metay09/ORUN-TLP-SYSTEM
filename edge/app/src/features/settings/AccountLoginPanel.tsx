// ORUN S6B PILOT ONLY: explicit account login on a previously enrolled phone.
// No claim that herd, history or zone drafts have been synchronized.
// Account bearer exists only in JS process memory; password is never persisted.
import { useEffect, useState } from "react";
import { isMobileIdentityAvailable } from "../../platform/orunMobileIdentity.ts";
import {
  activeMobileSession, mobileSessionStatus,
  type MobileSessionMeta,
} from "../../platform/orunMobileSession.ts";
import {
  pilotAccountLogin, pilotAccountLogout, pilotAccountReadShared,
  pilotAccountStatus,
} from "../../app/ownerAccountPilotRuntime.ts";
import type { AccountIdentity } from "../../app/ownerAccountClient.ts";
import { Card, KV, Section } from "../../ds/components.tsx";

function userMessage(error: unknown): string {
  // Never put raw Android exception details or server responses in the UI.
  if (error instanceof Error && error.message.length <= 180 &&
      !error.message.startsWith("ORUN_")) return error.message;
  return "Hesap işlemi tamamlanamadı. Telefon kaydını ve sunucuyu kontrol et.";
}

export function AccountLoginPanel() {
  const [expanded, setExpanded] = useState(false);
  const [device, setDevice] = useState<MobileSessionMeta | null>(null);
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [account, setAccount] = useState<AccountIdentity | null>(
    () => pilotAccountStatus());
  const [lastPage, setLastPage] = useState<{
    received: number; cursor: number; more: boolean;
  } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  useEffect(() => {
    if (!expanded || !isMobileIdentityAvailable()) return;
    let active = true;
    mobileSessionStatus().then((value) => {
      if (active) setDevice(value);
    }).catch(() => {
      if (active) {
        setDevice({ present: false });
        setError("Telefonun korunan yetkisi okunamadı.");
      }
    });
    return () => { active = false; };
  }, [expanded]);

  if (!isMobileIdentityAvailable()) return null;

  async function execute(job: () => Promise<void>): Promise<void> {
    if (busy) return;
    setBusy(true);
    setError("");
    setNotice("");
    try { await job(); }
    catch (caught: unknown) { setError(userMessage(caught)); }
    finally { setBusy(false); }
  }

  async function signIn(): Promise<void> {
    // Clear the visible password immediately after the user presses login.
    const secret = password;
    setPassword("");
    await execute(async () => {
      const native = await activeMobileSession();
      if (!native) throw new Error(
        "Önce bu telefonu sahibin onayıyla ORUN'a kaydetmelisin.");
      const result = await pilotAccountLogin(native, username, secret);
      setAccount(result);
      setLastPage(null);
      setNotice(
        "Hesap doğrulandı. Henüz hayvanlar veya tüm mera kayıtları "
        + "bu telefona eşitlenmedi.");
    });
  }

  async function readOnePage(): Promise<void> {
    await execute(async () => {
      const page = await pilotAccountReadShared();
      setAccount(pilotAccountStatus());
      setLastPage(page);
      setNotice("Sunucudan ortak veri ilk sayfası kontrol edildi. "
                + "Bu işlem henüz telefona kayıtları indirmez.");
    });
  }

  async function signOut(): Promise<void> {
    setPassword("");
    await execute(async () => {
      let confirmedOnServer = true;
      try {
        await pilotAccountLogout();
      } catch (cause: unknown) {
        // If the in-memory login still exists, the logout was refused
        // before discarding it (e.g. concurrent login), so report failure.
        if (pilotAccountStatus()) throw cause;
        confirmedOnServer = false;
      }
      setAccount(null);
      setLastPage(null);
      setNotice(confirmedOnServer
        ? "Hesap oturumu kapatıldı. Telefonun cihaz onayı korunur."
        : "Bu telefondaki hesap oturumu kapatıldı; sunucuda iptal "
          + "doğrulanamadı. Yetki süresi dolana kadar geçerli olabilir.");
    });
  }

  return (
    <>
      <Section title="Kullanıcı hesabı · pilot" />
      <Card>
        <div className="stack" style={{ padding: 16 }}>
          <p className="fine" style={{ margin: 0 }}>
            Bu ekran, Android'de kayıtlı telefonla gerçek hesap kimliği
            doğrulama denemesidir. Sürü, konum geçmişi ve tüm meralar
            henüz otomatik eşitlenmez. Sunucuda hesap hizmeti varsayılan
            olarak kapalıdır.
          </p>
          <button className="btn wide" type="button" disabled={busy}
            onClick={() => {
              if (expanded) {
                // Never keep a hidden password after closing the form.
                setPassword("");
                setError("");
              } else {
                setAccount(pilotAccountStatus());
              }
              setExpanded(!expanded);
            }}>
            {expanded ? "Hesap bölümünü kapat" : "Hesap girişini aç"}
          </button>
          {expanded && (
            <>
              <KV k="Telefon yetkisi">
                {device === null ? "Kontrol ediliyor"
                  : device.present ? "Onaylı · " + device.site
                  : "Önce telefon kaydı gerekli"}
              </KV>
              {account ? (
                <>
                  <KV k="Hesap">{account.username}</KV>
                  <KV k="Alan">{account.site}</KV>
                  <KV k="Yetki bitişi">
                    {new Date(account.expiresAt * 1000).toLocaleString("tr-TR")}
                  </KV>
                  <button className="btn" type="button" disabled={busy}
                    onClick={readOnePage}>
                    Sunucudaki ortak veri ilk sayfasını kontrol et
                  </button>
                  {lastPage && (
                    <>
                      <KV k="İlk sayfadaki değişiklik">{lastPage.received}</KV>
                      <KV k="Daha fazla sayfa">{lastPage.more ? "Var" : "Yok"}</KV>
                      <p className="fine">
                        Bu yalnız sunucudan yapılan doğrulama sorgusudur;
                        telefona kalıcı indirme veya mera uygulama işlemi değildir.
                      </p>
                    </>
                  )}
                  <button className="btn quiet" type="button" disabled={busy}
                    onClick={signOut}>Hesaptan çıkış yap</button>
                </>
              ) : (
                <form className="stack" autoComplete="off" onSubmit={(e) => {
                  e.preventDefault();
                  if (!busy && device?.present && username && password)
                    void signIn();
                }}>
                  <label className="lab">ORUN kullanıcı adı
                    <input className="inp" type="text" value={username}
                      minLength={3} maxLength={64}
                      autoCapitalize="off" autoCorrect="off"
                      autoComplete="username" spellCheck={false}
                      disabled={busy}
                      onChange={(e) => setUsername(e.target.value)} />
                  </label>
                  <label className="lab">Parola
                    <input className="inp" type="password" value={password}
                      minLength={12} maxLength={128}
                      autoComplete="current-password"
                      disabled={busy}
                      onChange={(e) => setPassword(e.target.value)} />
                  </label>
                  <button className="btn primary" type="submit"
                    disabled={busy || !device?.present ||
                              username.length < 3 || password.length < 12}>
                    {busy ? "Doğrulanıyor" : "Hesabımla giriş yap"}
                  </button>
                  <p className="fine">
                    Kullanıcı adı ve parola telefonun veri deposuna yazılmaz.
                    Oturum yetkisi yalnız uygulama açıkken bellekte tutulur.
                  </p>
                </form>
              )}
              {notice && <p role="status" className="sm">{notice}</p>}
              {error && <p role="alert" className="sm t-aging">{error}</p>}
            </>
          )}
        </div>
      </Card>
    </>
  );
}
