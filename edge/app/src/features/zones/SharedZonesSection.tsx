// S6F pilot: account-scoped, persisted server drafts in the Alanlar tab.
// No local editor writes, no automatic network access, no Tracker applied claim.
import { useEffect, useState } from "react";
import {
  pilotAccountStatus, pilotAccountStoredSnapshot,
} from "../../app/ownerAccountPilotRuntime.ts";
import { sharedZonePreviews, type SharedZoneList } from "../../app/sharedZonePreview.ts";
import { isMobileIdentityAvailable } from "../../platform/orunMobileIdentity.ts";
import { Card, Section } from "../../ds/components.tsx";

interface View {
  scope: string;
  list: SharedZoneList;
  cursor: number;
  groups: number;
  targets: number;
}

function scopeOf(account: NonNullable<ReturnType<typeof pilotAccountStatus>>): string {
  return [account.site, account.actor, account.installation,
          account.username, account.expiresAt].join("/");
}

export function SharedZonesSection() {
  const account = pilotAccountStatus();
  const scope = account ? scopeOf(account) : null;
  const [view, setView] = useState<View | null>(null);
  const [reload, setReload] = useState(0);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState(false);

  useEffect(() => {
    if (!scope || !isMobileIdentityAvailable()) return;
    let valid = true;
    setBusy(true);
    setError(false);
    setView(null);
    // Read only the existing SQLite mirror. Never start HTTP on tab navigation.
    pilotAccountStoredSnapshot().then((snapshot) => {
      const current = pilotAccountStatus();
      if (!valid || !current || scopeOf(current) !== scope) return;
      setView({
        scope,
        list: sharedZonePreviews(snapshot),
        cursor: snapshot.cursor,
        groups: snapshot.groups,
        targets: snapshot.targets,
      });
    }).catch(() => {
      if (valid) setError(true);
    }).finally(() => {
      if (valid) setBusy(false);
    });
    return () => { valid = false; };
  }, [scope, reload]);

  if (!isMobileIdentityAvailable()) return null;

  const shown = view?.scope === scope ? view : null;
  return (
    <>
      <Section title="Sunucudaki meralar · salt okunur" />
      <Card pad>
        <div className="stack">
          <p className="fine" style={{ margin: 0 }}>
            Bunlar sunucudan daha önce indirilip bu telefonda saklanan hesap
            taslaklarıdır. Yerel alanların üzerine yazılmaz; hiçbir tasmaya
            gönderilmiş veya uygulanmış sayılmaz.
          </p>
          {!scope ? (
            <p className="sm">Bu kayıtları görmek için Ayarlar bölümünde
              onaylı telefonunla hesabına giriş yap.</p>
          ) : (
            <>
              <button className="btn small" type="button" disabled={busy}
                onClick={() => setReload((value) => value + 1)}>
                {busy ? "Okunuyor" : "Telefondaki sunucu kayıtlarını yenile"}
              </button>
              {error && <p className="sm t-alarm" role="alert">
                Hesabın kayıtlı meraları okunamadı. Kayıtlar değiştirilmedi.
              </p>}
              {shown && (
                <>
                  <span className="sm strong">
                    {shown.list.total} mera · {shown.groups} grup ·
                    {shown.targets} hedef taslağı
                  </span>
                  <span className="fine">
                    Kaydedilen son sunucu sırası: {shown.cursor}.
                    Bu ekran yeni veri indirmez.
                  </span>
                  {shown.list.total === 0 &&
                    <p className="sm">Bu hesap için telefonda kayıtlı mera
                      taslağı bulunmuyor.</p>}
                  {shown.list.entries.map((zone) => (
                    <div key={zone.id} className="stack"
                      style={{ borderTop: "1px solid var(--line)", paddingTop: 10, gap: 3 }}>
                      <span className="sm strong">{zone.name}</span>
                      <span className="fine">
                        {zone.hectares.toLocaleString("tr-TR",
                          { maximumFractionDigits: 2 })} ha ·
                        {zone.vertices} köşe · Revizyon {zone.revision} ·
                        {zone.active ? "Taslakta açık" : "Taslakta kapalı"}
                      </span>
                    </div>
                  ))}
                  {shown.list.total > shown.list.entries.length && (
                    <span className="fine">
                      İlk {shown.list.entries.length} mera gösteriliyor.
                    </span>
                  )}
                  {shown.list.invalid > 0 && (
                    <p className="sm t-alarm" role="alert">
                      {shown.list.invalid} bozuk mera kaydı görüntülenmedi.
                      Kayıtlar değiştirilmedi.
                    </p>
                  )}
                </>
              )}
            </>
          )}
        </div>
      </Card>
    </>
  );
}
