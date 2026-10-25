import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import type { Controller } from "./controller.ts";

const STATE_TEXT: Record<string, string> = {
  idle: "Bağlı değil",
  searching: "Gateway aranıyor…",
  connecting: "Bağlanıyor…",
  connected: "Bağlı",
  waiting: "Yeniden bağlanmayı bekliyor",
};

function Stat({ label, value, tone }: { label: string; value: string | number; tone?: string }) {
  return (
    <div className={"stat" + (tone ? " " + tone : "")}>
      <span className="stat-value">{value}</span>
      <span className="stat-label">{label}</span>
    </div>
  );
}

function ServerCard({ controller }: { controller: Controller }) {
  const sv = useSyncExternalStore((fn) => controller.subscribe(fn), () => controller.snap.server);
  const [url, setUrl] = useState(sv.url);
  const [token, setToken] = useState(sv.token);
  const [auto, setAuto] = useState(sv.auto);
  const dirty = url !== sv.url || token !== sv.token || auto !== sv.auto;
  const c = sv.counts;
  return (
    <div className="card server">
      <div className="card-title">Sunucu</div>
      <label>Adres<input type="url" inputMode="url" autoCapitalize="off" autoCorrect="off"
        placeholder="http://100.114.36.9:8090" value={url} onChange={(e) => setUrl(e.target.value)} /></label>
      <label>Anahtar<input type="password" autoCapitalize="off" autoCorrect="off"
        value={token} onChange={(e) => setToken(e.target.value)} /></label>
      <label className="check"><input type="checkbox" checked={auto} onChange={(e) => setAuto(e.target.checked)} />
        Otomatik gönder</label>
      <div className="actions row">
        <button disabled={!dirty} onClick={() => controller.saveServer(url, token, auto)}>Kaydet</button>
        <button disabled={!sv.url || !sv.token || sv.busy} onClick={() => controller.sync(true)}>
          {sv.busy ? "Gönderiliyor…" : "Şimdi gönder"}</button>
      </div>
      <div className="detail-line">
        {c ? <>Sunucuda <b>{c.uploaded}</b> · bekleyen <b className={c.pending ? "warn" : ""}>{c.pending}</b>
          {c.conflict + c.rejected > 0 && <> · <span className="bad">çakışma {c.conflict} · red {c.rejected}</span></>}</> : "—"}
        {sv.lastOk && <> · sunucuyla son temas {sv.lastOk}</>}
      </div>
      {sv.error && <div className="detail">{sv.error}</div>}
    </div>
  );
}

export function App({ controller }: { controller: Controller }) {
  const s = useSyncExternalStore((fn) => controller.subscribe(fn), () => controller.snap);
  const [tab, setTab] = useState<"map" | "link">("link");
  const logEnd = useRef<HTMLDivElement>(null);
  const map = useRef<HTMLIFrameElement>(null);

  useEffect(() => { if (tab === "link") logEnd.current?.scrollIntoView({ block: "end" }); }, [s.log, tab]);
  // New records show on the map at once, not on its next 10 s poll.
  useEffect(() => {
    if (tab !== "map") return;
    try { (map.current?.contentWindow as { orunRefresh?: () => void } | null)?.orunRefresh?.(); } catch { /* loading */ }
  }, [tab, s.records, s.handedOff]);

  const busy = s.link !== "none";
  const dot = s.state === "connected" ? "live" : s.state === "idle" ? "" : "wait";
  const c = s.counts;

  return (
    <div className="app">
      <header className="top">
        <div className="brand"><b>ORUN</b><span>Edge · masa testi</span></div>
        <div className="link-state" aria-live="polite">
          <span className={"dot " + dot} />
          <span>{s.link === "ble" ? "Bluetooth: " : s.link === "usb" ? "USB: " : ""}{STATE_TEXT[s.state]}</span>
        </div>
        <nav className="tabs" role="tablist">
          <button role="tab" aria-selected={tab === "link"} onClick={() => setTab("link")}>Bağlantı</button>
          <button role="tab" aria-selected={tab === "map"} onClick={() => setTab("map")}>Harita</button>
        </nav>
      </header>

      <main className={"content " + tab}>
        <section className="link-view" hidden={tab !== "link"}>
          {s.error && <div className="banner bad">Kayıt defteri açılamadı: {s.error}</div>}
          <div className="card">
            <div className="actions">
              {!busy && <>
                <button className="primary" disabled={!s.ready} onClick={() => controller.connectBle(false)}>Bluetooth ile bağlan</button>
                <button disabled={!s.ready} onClick={() => controller.connectUsb()}>USB ile bağlan</button>
                <button className="quiet" disabled={!s.ready} onClick={() => controller.connectBle(true)}>Başka gateway seç</button>
              </>}
              {busy && <button className="danger" onClick={() => controller.disconnect()}>Bağlantıyı kes</button>}
            </div>
            {s.detail && s.state !== "connected" && <div className="detail">{s.detail}</div>}
            <div className="gw">
              <span>Gateway</span><code>{s.gateway ?? "—"}</code>
              {s.gatewayHeld && <span className="muted">tuttuğu {s.gatewayHeld}</span>}
            </div>
          </div>

          <div className="stats">
            <Stat label="yeni" value={c.STORED} tone="ok" />
            <Stat label="vardı" value={c.DUPLICATE} />
            <Stat label="teslim" value={s.handedOff} tone="ok" />
            <Stat label="bekleyen teslim" value={s.pending} tone={s.pending ? "warn" : undefined} />
            <Stat label="red" value={c.REJECTED + s.rejected} tone={c.REJECTED + s.rejected ? "bad" : undefined} />
            <Stat label="telefonda kayıt" value={s.records ?? "—"} />
          </div>
          <ServerCard controller={controller} />

          {(c.CONFLICT > 0 || c.UNCERTAIN > 0) &&
            <div className="banner warn">Çakışma {c.CONFLICT} · belirsiz {c.UNCERTAIN}: bu kayıtlar teslim alınmadı, gateway'de kalır.</div>}

          <div className="log" role="log" aria-label="Kayıt akışı">
            {s.log.length === 0 && <div className="muted">Gateway'e bağlanınca gelen kayıtlar burada görünür.</div>}
            {s.log.map((l, i) => <div key={i} className={l.includes("UYARI") || l.includes(" RED ") ? "bad" : l.includes(" YENİ") ? "ok" : ""}>{l}</div>)}
            <div ref={logEnd} />
          </div>
          <p className="foot">
            Telefon, kaydı kendi defterine yazdıktan sonra gateway'e “aldım” der; gateway ancak o zaman yerini boşaltır.
            Masa testi: herkese açık test anahtarı kullanılır. {s.storage}
          </p>
        </section>

        {/* Kept mounted so the map keeps its view while switching tabs. */}
        <iframe ref={map} className="map" title="ORUN harita" src="/map.html" hidden={tab !== "map"} />
      </main>
    </div>
  );
}
