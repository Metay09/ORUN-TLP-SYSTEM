// ORUN S6B PILOT ONLY: explicit account login on a previously enrolled phone.
// No claim that herd, history or zone drafts have been synchronized.
// Account bearer exists only in JS process memory; password is never persisted.
import { useState } from "react";
import {
  pilotPasswordLogin, pilotAccountLogout, pilotAccountStatus,
  pilotAccountDownloadShared, pilotAccountStoredSnapshot,
} from "../../app/ownerAccountPilotRuntime.ts";
import type { AccountIdentity } from "../../app/ownerAccountClient.ts";
import type {
  SharedMirrorSnapshot, MirrorPullResult,
} from "../../app/accountSharedMirror.ts";
import { Card, KV, Section } from "../../ds/components.tsx";

function userMessage(error: unknown): string {
  // Never put raw Android exception details or server responses in the UI.
  if (error instanceof Error && error.message.length <= 180 &&
      !error.message.startsWith("ORUN_")) return error.message;
  return "Hesap işlemi tamamlanamadı. Telefon kaydını ve sunucuyu kontrol et.";
}

export function AccountLoginPanel() {
  const [expanded, setExpanded] = useState(true);
  const [username, setUsername] = useState("owner");
  const [password, setPassword] = useState("");
  const [account, setAccount] = useState<AccountIdentity | null>(
    () => pilotAccountStatus());
  const [saved, setSaved] = useState<SharedMirrorSnapshot | null>(null);
  const [lastPull, setLastPull] = useState<MirrorPullResult | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

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
      const result = await pilotPasswordLogin(username, secret);
      setAccount(result);
      setLastPull(null);
      const stored = await pilotAccountStoredSnapshot();
      setSaved(stored);
      setNotice(
        "Hesap doğrulandı. Önceden indirilmiş ortak veri taslakları "
        + "gösteriliyor; hayvan ve konum geçmişi henüz eşitlenmedi.");
    });
  }

  async function downloadDrafts(): Promise<void> {
    await execute(async () => {
      const result = await pilotAccountDownloadShared();
      const stored = await pilotAccountStoredSnapshot();
      setAccount(pilotAccountStatus());
      setLastPull(result);
      setSaved(stored);
      setNotice(result.more
        ? "İndirilen sayfalar SQLite'a kaydedildi. Sunucuda daha fazla "
          + "kayıt var; devam etmek için yeniden indir."
        : "Sunucudaki mevcut ortak veri taslakları SQLite'a kalıcı kaydedildi. "
          + "Bunlar tasmalara uygulanmış mera komutları değildir.");
    });
  }

  async function showStored(): Promise<void> {
    await execute(async () => {
      setSaved(await pilotAccountStoredSnapshot());
      setNotice("Bu telefonda önceden kalıcı saklanan taslaklar gösteriliyor.");
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
      setSaved(null);
      setLastPull(null);
      setNotice(confirmedOnServer
        ? "Hesap oturumu kapatıldı. Telefonun cihaz onayı korunur."
        : "Bu telefondaki hesap oturumu kapatıldı; sunucuda iptal "
          + "doğrulanamadı. Yetki süresi dolana kadar geçerli olabilir.");
    });
  }

  return (
    <>
      <Section title="Hesabım" />
      <Card>
        <div className="stack" style={{ padding: 16 }}>
          <p className="fine" style={{ margin: 0 }}>
            Kullanıcı adın ve parolanla giriş yap. Yeni telefonda
            ayrıca telefon kaydı, onay kodu veya anahtar gerekmiyor.
            HTTPS bağlantısı otomatik kullanılır. Sürü ve konum verileri
            henüz hesapla otomatik eşitlenmiyor.
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
              {account ? (
                <>
                  <KV k="Hesap">{account.username}</KV>
                  <KV k="Alan">{account.site}</KV>
                  <KV k="Yetki bitişi">
                    {new Date(account.expiresAt * 1000).toLocaleString("tr-TR")}
                  </KV>
                  <button className="btn primary" type="button" disabled={busy}
                    onClick={downloadDrafts}>
                    {busy ? "İndiriliyor" : "Mera ve grup taslaklarını SQLite'a indir"}
                  </button>
                  <button className="btn" type="button" disabled={busy}
                    onClick={showStored}>
                    Telefonda kayıtlı ortak verileri göster
                  </button>
                  {lastPull && (
                    <>
                      <KV k="Bu denemede indirilen değişiklik">{lastPull.received}</KV>
                      <KV k="Kaydedilen son sıra">{lastPull.cursor}</KV>
                      <KV k="Sunucuda devamı">{lastPull.more ? "Var" : "Yok"}</KV>
                    </>
                  )}
                  {saved && (
                    <>
                      <KV k="SQLite mera taslağı">{saved.zones}</KV>
                      <KV k="SQLite grup taslağı">{saved.groups}</KV>
                      <KV k="SQLite hedef taslağı">{saved.targets}</KV>
                      <KV k="Kalıcı kayıt imleci">{saved.cursor}</KV>
                      {Object.entries(saved.resources)
                        .filter(([key, item]) =>
                          key.startsWith("zone:") && !item.deleted &&
                          typeof item.body?.name === "string")
                        .slice(0, 5).map(([key, item]) =>
                          <KV key={key} k="Sunucudan kayıtlı mera">
                            {String(item.body?.name)}
                          </KV>)}
                      <p className="fine">
                        Buradaki kayıtlar sunucudan onaylanmış mera/grup
                        taslaklarıdır. Yerel mera düzenleyicisinin üzerine
                        yazılmaz; tasmalara gönderildi veya uygulandı
                        anlamına gelmez. Tekrar giriş olmadan gösterilmez.
                      </p>
                    </>
                  )}
                  <button className="btn quiet" type="button" disabled={busy}
                    onClick={signOut}>Hesaptan çıkış yap</button>
                </>
              ) : (
                <form className="stack" autoComplete="off" onSubmit={(e) => {
                  e.preventDefault();
                  if (!busy && username && password)
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
                    disabled={busy || username.length < 3 || password.length < 12}>
                    {busy ? "Doğrulanıyor" : "Hesabımla giriş yap"}
                  </button>
                  <p className="fine">
                    Parolan saklanmaz. Oturumun süresi dolunca kullanıcı adın ve parolanla yeniden giriş yaparsın.
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
