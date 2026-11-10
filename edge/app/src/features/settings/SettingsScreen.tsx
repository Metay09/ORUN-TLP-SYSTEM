// Ayarlar: hesap, internetsiz harita, görünüm ve hakkında.
import { tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import type { Theme } from "../../app/stores.ts";
import { Card, KV, Screen, Section, Seg, ToggleRow } from "../../ds/components.tsx";
import { AccountLoginPanel } from "./AccountLoginPanel.tsx";
import { OfflineMapPanel } from "./OfflineMapPanel.tsx";

declare const __APP_VERSION__: string;

export function SettingsScreen() {
  const m = useApp();
  const nav = useNav();
  useBack(() => nav.pop());
  const p = m.prefs;
  return (
    <Screen title={tr.settings.title} onBack={() => nav.pop()}>
      <AccountLoginPanel />
      <OfflineMapPanel />

      <Section title={tr.settings.look} />
      <Card>
        <div className="kv"><span>{tr.settings.theme}</span></div>
        <Seg<Theme> value={p.theme} onChange={(v) => m.actions.setPrefs({ theme: v })}
          options={(["auto", "light", "dark"] as Theme[]).map((t) => ({ value: t, label: tr.settings.themes[t] }))} />
        <div style={{ height: 8 }} />
        <ToggleRow title={tr.settings.sun} sub={tr.settings.sunHelp} on={p.sun} onChange={(v) => m.actions.setPrefs({ sun: v })} />
      </Card>

      <Section title={tr.settings.about} />
      <Card>
        <KV k={tr.settings.version}>{typeof __APP_VERSION__ === "string" ? __APP_VERSION__ : "—"}</KV>
        <p className="fine" style={{ padding: "12px 16px 14px", margin: 0 }}>{tr.settings.sources}</p>
      </Card>
    </Screen>
  );
}
