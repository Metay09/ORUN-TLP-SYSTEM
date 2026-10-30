// ORUN S5C candidate: explicitly owner-approved phone enrollment.
// This is NOT account login, not live herd sync and not permission to send
// Tracker/geofence commands. Hidden on web; pilot-only on native Android.
// The shared Backend has enrollment disabled by default.
import { useEffect, useState } from "react";
import {
  mobileIdentity, isMobileIdentityAvailable, signMobileEnrollment,
  type OrunMobilePublicIdentity,
} from "../../platform/orunMobileIdentity.ts";
import {
  beginPhoneEnrollment, beginPhoneRecovery,
  type PhoneEnrollmentPending, type PhonePublicIdentity,
} from "../../app/phoneEnrollment.ts";
import {
  completePhoneEnrollment, completePhoneRecovery,
} from "../../app/completePhoneEnrollment.ts";
import {
  storeMobileSession, mobileSessionStatus, clearLocalMobileSession,
  type MobileSessionMeta,
} from "../../platform/orunMobileSession.ts";
import { pilotAccountLogout, pilotAccountStatus } from "../../app/ownerAccountPilotRuntime.ts";
import { Card, KV, Section } from "../../ds/components.tsx";

function readableError(error: unknown): string {
  // Native errors may contain provider details; use bounded safe language.
  if (!(error instanceof Error)) return "İşlem tamamlanamadı. Tekrar deneyin.";
  const message = error.message;
  if (message.startsWith("ORUN_"))
    return "Bu telefonda güvenli kayıt işlemi kullanılamıyor. Cihaz anahtarı kontrol edilmeli.";
  return message.length < 220
    ? message : "İşlem tamamlanamadı. Kayıt durumunu kontrol edin.";
}
function publicIdentity(value: OrunMobilePublicIdentity): PhonePublicIdentity {
  if (!value.present || !value.installationId || !value.fingerprint ||
      !value.publicKeyBase64 || value.algorithm !== "P256-SHA256")
    throw new Error("Güvenli telefon kimliği mevcut değil.");
  return {
    algorithm: value.algorithm, installationId: value.installationId,
    fingerprint: value.fingerprint, publicKeyBase64: value.publicKeyBase64,
  };
}

export function PhoneRegistrationPanel() {
  const [expanded, setExpanded] = useState(false);
  const [serverUrl, setServerUrl] = useState("");
  const [identity, setIdentity] = useState<PhonePublicIdentity | null>(null);
  const [session, setSession] = useState<MobileSessionMeta | null>(null);
  const [pending, setPending] = useState<PhoneEnrollmentPending | null>(null);
  const [ownerCode, setOwnerCode] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [message, setMessage] = useState("");

  // Never run on a browser. Never generate a key during a passive read.
  useEffect(() => {
    if (!isMobileIdentityAvailable() || !expanded) return;
    let cancelled = false;
    Promise.all([mobileIdentity(false), mobileSessionStatus()])
      .then(([key, value]) => {
        if (!cancelled) {
          setIdentity(key.present ? publicIdentity(key) : null);
          setSession(value);
        }
      })
      .catch((e: unknown) => { if (!cancelled) setError(readableError(e)); });
    return () => { cancelled = true; };
  }, [expanded]);

  if (!isMobileIdentityAvailable()) return null;
  async function run(job: () => Promise<void>) {
    if (busy) return;
    setBusy(true);
    setError("");
    setMessage("");
    try { await job(); }
    catch (e: unknown) { setError(readableError(e)); }
    finally { setBusy(false); }
  }

  async function prepareKey() {
    await run(async () => {
      const key = publicIdentity(await mobileIdentity(true));
      setIdentity(key);
      setMessage("Telefonun güvenli anahtarı hazır. Sunucudan kayıt isteği oluşturabilirsin.");
    });
  }

  async function begin(recovery: boolean) {
    if (!identity) return;
    await run(async () => {
      const value = recovery
        ? await beginPhoneRecovery(serverUrl, identity)
        : await beginPhoneEnrollment(serverUrl, identity);
      setOwnerCode("");
      setPending(value);
      setMessage(
        "Bilgisayardaki sahip, aşağıdaki istek kimliği ve parmak izini "
        + "telefon ekranından doğrulayarak onaylamalıdır.");
    });
  }

  async function finish() {
    if (!identity || !pending) return;
    await run(async () => {
      const result = pending.recovery
        ? await completePhoneRecovery(
          serverUrl, identity,
          pending as PhoneEnrollmentPending & { recovery: true },
          ownerCode, signMobileEnrollment, storeMobileSession)
        : await completePhoneEnrollment(
          serverUrl, identity, pending, ownerCode,
          signMobileEnrollment, storeMobileSession);
      // Clear the one-time code even if the local session is unavailable.
      setOwnerCode("");
      setPending(null);
      setSession(await mobileSessionStatus());
      setMessage("Bu telefon, " + result.site +
                 " alanına ortak veriler için kaydedildi. Gerçek hesap girişi "
                 + "ve sürü eşitlemesi ayrı geliştirme aşamasındadır.");
    });
  }

  async function signOutLocal() {
    await run(async () => {
      // Do not leave a pilot account signed in after removing its underlying
      // device credential. A failed network revoke still clears account bearer
      // locally; the server token will time out separately if unreachable.
      if (pilotAccountStatus()) {
        try { await pilotAccountLogout(); }
        catch (err: unknown) {
          if (pilotAccountStatus()) throw err;
        }
      }
      await clearLocalMobileSession();
      setSession({ present: false });
      setPending(null);
      setOwnerCode("");
      setMessage("Yerel oturum silindi. Sunucudaki telefon yetkisi iptal edilmedi.");
    });
  }

  return (
    <>
      <Section title="Telefon erişimi · pilot" />
      <Card>
        <div style={{ padding: 16 }} className="stack">
          <p className="fine" style={{ margin: 0 }}>
            Güvenli telefon kaydı için geliştirme ekranı. Bu ekran kullanıcı
            hesabıyla giriş değildir. Hayvan, mera ve tasma verilerini
            otomatik getirmez. Kayıt sunucuda varsayılan olarak kapalıdır.
          </p>
          <button className="btn wide" onClick={() => {
            if (expanded) {
              // Never keep the owner code in a hidden form.
              setOwnerCode("");
              setPending(null);
            }
            setExpanded(!expanded);
          }} type="button">
            {expanded ? "Kayıt ayrıntılarını gizle" : "Pilot telefon kaydını aç"}
          </button>
        </div>
        {expanded && (
          <div style={{ padding: "0 16px 16px" }} className="stack">
            <KV k="Oturum">{session?.present
              ? "Bu telefon onaylı · " + session.site
              : session === null ? "Kontrol ediliyor" : "Kayıtlı oturum yok"}</KV>
            {session?.present && session.expiresAt &&
              <KV k="Yetki bitişi">{new Date(session.expiresAt * 1000).toLocaleString("tr-TR")}</KV>}
            <label className="lab">Ortak veri Backend HTTPS adresi
              <input className="inp" type="url" value={serverUrl}
                placeholder="https://ornek.ts.net:8443"
                inputMode="url" autoComplete="off" autoCapitalize="off"
                spellCheck={false} disabled={busy}
                onChange={(e) => setServerUrl(e.target.value.trim())} />
            </label>
            {identity ? (
              <>
                <KV k="Telefon anahtar izi"><code>{identity.fingerprint}</code></KV>
                <p className="fine">
                  Anahtar yalnız bu telefonda tutulur. Yeni telefona
                  aktarılmaz; yeni telefona ayrı onay verilir.
                </p>
              </>
            ) : (
              <button className="btn" type="button" disabled={busy}
                onClick={prepareKey}>Bu telefon için güvenli anahtar oluştur</button>
            )}
            {identity && !session?.present && !pending && (
              <>
                <button className="btn primary" type="button"
                  disabled={busy || !serverUrl.trim()}
                  onClick={() => begin(false)}>Yeni telefon kayıt isteği oluştur</button>
                <button className="btn" type="button"
                  disabled={busy || !serverUrl.trim()}
                  onClick={() => begin(true)}>
                  Daha önce kaydedilmiş telefonun yetkisini kurtar
                </button>
              </>
            )}
            {pending && (
              <>
                <p className="fine" style={{ margin: 0 }}>
                  {pending.recovery
                    ? "Yetki kurtarma: eski oturum anahtarı, onaylı işlem bitince geçersiz olur."
                    : "Yeni telefon kaydı: sahip onayı gereklidir."}
                </p>
                <KV k="Sahibe gösterilecek istek ID"><code>{pending.requestId}</code></KV>
                <KV k="Sahibe gösterilecek anahtar izi"><code>{pending.fingerprint}</code></KV>
                <label className="lab">Sahibin tek kullanımlık onay kodu
                  <input className="inp" type="password" value={ownerCode}
                    autoComplete="off" autoCapitalize="off" autoCorrect="off"
                    spellCheck={false} disabled={busy}
                    onChange={(e) => setOwnerCode(e.target.value)} />
                </label>
                <button className="btn primary" type="button"
                  disabled={busy || ownerCode.length < 30}
                  onClick={finish}>
                  {busy ? "Doğrulanıyor" : "Sahip onayıyla kaydı tamamla"}
                </button>
                <button className="btn quiet" type="button" disabled={busy}
                  onClick={() => { setPending(null); setOwnerCode(""); }}>
                  Bu istekten çık
                </button>
                <p className="fine">
                  Onay kodunu veya sunucudan alınan yetkiyi destek sohbetine,
                  ekran görüntüsüne ve uygulama günlüklerine koyma.
                </p>
              </>
            )}
            {session?.present && (
              <button className="btn quiet" type="button" disabled={busy}
                onClick={signOutLocal}>Yalnız bu telefondaki oturumu kaldır</button>
            )}
            {message && <p className="sm" role="status">{message}</p>}
            {error && <p className="sm t-aging" role="alert">{error}</p>}
          </div>
        )}
      </Card>
    </>
  );
}
