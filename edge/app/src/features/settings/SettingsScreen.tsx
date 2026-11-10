// Ayarlar: hesap, tasma ayarları, internetsiz harita, görünüm ve hakkında.
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import type { Theme } from "../../app/stores.ts";
import { Card, Header, KV, Section, Seg, ToggleRow } from "../../ds/components.tsx";
import { AccountLoginPanel } from "./AccountLoginPanel.tsx";
import { OfflineMapPanel } from "./OfflineMapPanel.tsx";
import { TrackerSettingsPanel } from "./TrackerSettingsPanel.tsx";

declare const __APP_VERSION__: string;

export function SettingsScreen() {
  const m = useApp();
  const p = m.prefs;
  return (
    <div className="page">
      <Header title={tr.settings.title} />
      <AccountLoginPanel />
      <TrackerSettingsPanel />
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
    </div>
  );
}
