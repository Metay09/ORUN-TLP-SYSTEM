// ORUN hesabı: kullanıcı adı ya da e-posta + parola. Giriş bu telefonda açık
// kalır (Android'in korumalı anahtar deposunda şifreli); parola saklanmaz.
import { useEffect, useState } from "react";
import {
  onPilotAccountChange, pilotAccountEmail, pilotAccountLogout,
  pilotAccountStatus, pilotPasswordLogin, pilotSetEmail,
} from "../../app/ownerAccountPilotRuntime.ts";
import { Card, KV, Section } from "../../ds/components.tsx";

function userMessage(error: unknown): string {
  // Never put raw Android exception details or server responses in the UI.
  if (error instanceof Error && error.message.length <= 180 &&
      !error.message.startsWith("ORUN_")) return error.message;
  return "Hesap işlemi tamamlanamadı. Sunucu bağlantısını kontrol et.";
}

export function AccountLoginPanel() {
  const [login, setLogin] = useState("");
  const [password, setPassword] = useState("");
  const [email, setEmail] = useState("");
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const [savedEmail, setSavedEmail] = useState(() => pilotAccountEmail());
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  useEffect(() => onPilotAccountChange(() => {
    setAccount(pilotAccountStatus());
    setSavedEmail(pilotAccountEmail());
  }), []);

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
      setAccount(await pilotPasswordLogin(login, secret));
      setNotice("Giriş yapıldı. Bu telefonda açık kalacak; konumlar kendiliğinden gelir.");
    });
  }

  async function addEmail(): Promise<void> {
    await execute(async () => {
      setSavedEmail(await pilotSetEmail(email));
      setEmail("");
      setNotice("E-posta eklendi. Artık e-postanla da giriş yapabilirsin.");
    });
  }

  async function signOut(): Promise<void> {
    await execute(async () => {
      try {
        await pilotAccountLogout();
      } catch (cause: unknown) {
        if (pilotAccountStatus()) throw cause;
      }
      setAccount(null);
      setNotice("Çıkış yapıldı.");
    });
  }

  return (
    <>
      <Section title="Hesabım" />
      <Card>
        <div className="stack" style={{ padding: 16 }}>
          {account ? (
            <>
              <KV k="Hesap">{account.username}</KV>
              <KV k="E-posta">{savedEmail ?? "Eklenmedi"}</KV>
              {!savedEmail && (
                <form className="stack" onSubmit={(e) => {
                  e.preventDefault();
                  if (!busy && email) void addEmail();
                }}>
                  <label className="lab">E-posta (girişte kullanıcı adı yerine)
                    <input className="inp" type="email" value={email} maxLength={254}
                      autoCapitalize="off" autoCorrect="off" spellCheck={false}
                      autoComplete="email" disabled={busy}
                      onChange={(e) => setEmail(e.target.value)} />
                  </label>
                  <button className="btn" type="submit" disabled={busy || !email.includes("@")}>
                    E-posta ekle
                  </button>
                </form>
              )}
              <p className="fine">Bu telefonda giriş açık kalır. Çıkış yapana kadar parola sorulmaz.</p>
              <button className="btn quiet" type="button" disabled={busy}
                onClick={signOut}>Çıkış yap</button>
            </>
          ) : (
            <form className="stack" autoComplete="off" onSubmit={(e) => {
              e.preventDefault();
              if (!busy && login && password) void signIn();
            }}>
              <label className="lab">Kullanıcı adı veya e-posta
                <input className="inp" type="text" value={login}
                  minLength={3} maxLength={254}
                  autoCapitalize="off" autoCorrect="off"
                  autoComplete="username" spellCheck={false}
                  disabled={busy}
                  onChange={(e) => setLogin(e.target.value)} />
              </label>
              <label className="lab">Parola
                <input className="inp" type="password" value={password}
                  minLength={12} maxLength={128}
                  autoComplete="current-password"
                  disabled={busy}
                  onChange={(e) => setPassword(e.target.value)} />
              </label>
              <button className="btn primary" type="submit"
                disabled={busy || login.length < 3 || password.length < 12}>
                {busy ? "Doğrulanıyor" : "Giriş yap"}
              </button>
              <p className="fine">Parolan saklanmaz. Giriş bu telefonda açık kalır.</p>
            </form>
          )}
          {notice && <p role="status" className="sm">{notice}</p>}
          {error && <p role="alert" className="sm t-aging">{error}</p>}
        </div>
      </Card>
    </>
  );
}
