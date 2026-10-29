// Ayarlar: look (theme, sun mode), the sample herd, data and about.
import { useEffect, useState } from "react";
import { CaretRight } from "@phosphor-icons/react";
import { tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import type { Theme } from "../../app/stores.ts";
import { Card, Cell, KV, Screen, Section, Seg, ToggleRow } from "../../ds/components.tsx";
import { serverWords } from "../network/words.ts";
import { identityFault, inspectIdentity, isAndroidIdentityAvailable, type AndroidIdentity, type IdentityFault } from "../../platform/orunIdentity.ts";

declare const __APP_VERSION__: string;

export function SettingsScreen() {
  const m = useApp();
  const nav = useNav();
  useBack(() => nav.pop());
  const p = m.prefs;
  const androidIdentity = isAndroidIdentityAvailable();
  const [identity, setIdentity] = useState<AndroidIdentity | null>(null);
  const [identityBusy, setIdentityBusy] = useState(false);
  const [identityError, setIdentityError] = useState<IdentityFault | null>(null);
  useEffect(() => {
    if (!androidIdentity) return;
    let live = true;
    void inspectIdentity(false)
      .then((result) => { if (live) setIdentity(result); })
      .catch((error: unknown) => { if (live) setIdentityError(identityFault(error)); });
    return () => { live = false; };
  }, [androidIdentity]);
  const prepareIdentity = async () => {
    if (identityBusy) return;
    setIdentityBusy(true);
    setIdentityError(null);
    try {
      // Only an explicit button tap can create a non-exportable key.
      setIdentity(await inspectIdentity(!identity?.present));
    } catch (error: unknown) {
      setIdentityError(identityFault(error));
    } finally {
      setIdentityBusy(false);
    }
  };
  return (
    <Screen title={tr.settings.title} onBack={() => nav.pop()}>
      <Section title={tr.settings.look} />
      <Card>
        <div className="kv"><span>{tr.settings.theme}</span></div>
        <Seg<Theme> value={p.theme} onChange={(v) => m.actions.setPrefs({ theme: v })}
          options={(["auto", "light", "dark"] as Theme[]).map((t) => ({ value: t, label: tr.settings.themes[t] }))} />
        <div style={{ height: 8 }} />
        <ToggleRow title={tr.settings.sun} sub={tr.settings.sunHelp} on={p.sun} onChange={(v) => m.actions.setPrefs({ sun: v })} />
      </Card>

      <Section title={tr.settings.data} />
      <Card>
        {m.canLeaveDemo
          ? <ToggleRow title={tr.settings.demo} sub={tr.settings.demoHelp} on={m.demo} onChange={(v) => m.actions.setPrefs({ demo: v })} />
          : <Cell title={tr.settings.demo} sub={tr.settings.demoHelp} wrapSub />}
        <Cell title={tr.network.server} sub={serverWords(m.server).text} trail={<CaretRight size={18} color="var(--mute)" />}
          onClick={() => nav.setTab("network")} />
        {!m.demo && <Cell title={tr.settings.phoneRecords(m.snap.records ?? 0)} />}
      </Card>

      {androidIdentity && <>
        <Section title={tr.settings.identityTitle} />
        <Card pad>
          <div className="stack">
            <span className="fine">{tr.settings.identityHelp}</span>
            {identityBusy ? <span className="sm" role="status">{tr.settings.identityLoading}</span>
              : identityError ? <span className="sm t-alarm" role="alert">{tr.settings.identityFailed} <code>{identityError}</code></span>
              : <span className="sm" role="status">{identity?.present
                ? tr.settings.identityReady : tr.settings.identityMissing}</span>}
            {identity?.present && <div className="kv">
              <span className="fine">{tr.settings.identityFingerprint}</span>
              <code>{identity.fingerprint}</code>
            </div>}
            <button className="btn small" type="button" disabled={identityBusy}
              onClick={() => void prepareIdentity()}>
              {identity?.present ? tr.settings.identityRefresh : tr.settings.identityCreate}
            </button>
          </div>
        </Card>
      </>}

      <Section title={tr.settings.about} />
      <Card>
        <KV k={tr.settings.version}>{typeof __APP_VERSION__ === "string" ? __APP_VERSION__ : "—"}</KV>
        <p className="fine" style={{ padding: "12px 16px 14px", margin: 0 }}>{tr.settings.sources}</p>
      </Card>
    </Screen>
  );
}
