// ORUN phone app, UX v2: map first. Three destinations (Harita, Sürü, Defter);
// the station/server link lives behind the status pill. Plain Turkish, one
// sentence and one colour per animal; engineering detail only under
// "Teknik ayrıntılar".
import { useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore, type ReactNode } from "react";
import { App as CapApp } from "@capacitor/app";
import {
  ArrowSquareOut, BatteryFull, BatteryLow, BatteryMedium, BatteryWarning, Bluetooth, CaretRight,
  CheckCircle, CloudArrowUp, CloudCheck, CloudSlash, CornersOut, Cow, Crosshair, MapTrifold,
  NavigationArrow, Notebook, PencilSimple, Plus, Stack, Usb, Warning, X,
} from "@phosphor-icons/react";
import type { Controller, JournalEntry, Snapshot } from "./controller.ts";
import type { PointsResult } from "../core/points.ts";
import {
  bearingDeg, buildHerd, clock, directionName, distanceM, distanceText, herdSummary, shortId,
  type Animal,
} from "./herd.ts";
import { initialsOf, MapView, type Layer, type MyPosition } from "./MapView.tsx";
import { ProfileStore } from "./profiles.ts";
import { Sheet, type Snap } from "./Sheet.tsx";

type Tab = "harita" | "suru" | "defter";
const profiles = new ProfileStore();
const LAYER_KEY = "orun.edge.layer";
const LAYER_NAMES: Record<Layer, string> = { uydu: "Uydu", harita: "Harita", arazi: "Arazi" };

// ---------------------------------------------------------------- helpers --
function useSnap(c: Controller): Snapshot {
  return useSyncExternalStore((fn) => c.subscribe(fn), () => c.snap);
}

function usePoints(c: Controller, version: number): PointsResult | null {
  const [data, setData] = useState<PointsResult | null>(null);
  const last = useRef(0);
  useEffect(() => {
    let cancelled = false;
    // Bursts of records: at most one ledger read every 2 s.
    const wait = Math.max(0, 2000 - (Date.now() - last.current));
    const t = setTimeout(async () => {
      last.current = Date.now();
      const r = await c.points().catch(() => null);
      if (!cancelled && r) setData(r);
    }, wait);
    return () => { cancelled = true; clearTimeout(t); };
  }, [c, version]);
  return data;
}

function useNow(ms: number): number {
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const t = setInterval(() => setNow(Date.now()), ms); return () => clearInterval(t); }, [ms]);
  return now;
}

function loadLayer(): Layer {
  try {
    const v = localStorage.getItem(LAYER_KEY);
    return v === "harita" || v === "arazi" ? v : "uydu";
  } catch { return "uydu"; }
}

function Avatar({ a, size = 44 }: { a: Animal; size?: number }) {
  return (
    <span className={"avatar " + a.freshness} style={{ ["--pin" as string]: a.color, width: size, height: size }}>
      {a.named ? initialsOf(a.name) : <Cow size={Math.round(size * 0.55)} weight="bold" />}
    </span>
  );
}

function BatteryIcon({ a }: { a: Animal }) {
  const p = a.batteryPercent;
  if (a.battery === "critical") return <BatteryWarning size={20} weight="fill" className="bad" />;
  if (a.battery === "low") return <BatteryLow size={20} className="warn" />;
  if (p !== null && p >= 70) return <BatteryFull size={20} className="soft" />;
  if (p !== null) return <BatteryMedium size={20} className="soft" />;
  return null;
}

// --------------------------------------------------------------- status pill --
function linkWords(s: Snapshot): { dot: string; text: string } {
  if (!s.ready) return { dot: "grey", text: s.error ? "Telefon kayıt defteri açılamadı" : "Açılıyor…" };
  if (s.link !== "none" && s.state === "connected") return { dot: "ok", text: "İstasyona bağlı" };
  if (s.link !== "none") return { dot: "wait", text: s.state === "waiting" ? "İstasyon bekleniyor" : "İstasyon aranıyor…" };
  return { dot: "grey", text: "İstasyona bağlı değil" };
}

function syncWords(s: Snapshot): { icon: "ok" | "up" | "off" | null; text: string } {
  const sv = s.server, c = sv.counts;
  if (!sv.url || !sv.token) return { icon: null, text: "Sunucu ayarlanmadı · kayıtlar telefonda saklı" };
  if (sv.busy) return { icon: "up", text: "Gönderiliyor…" };
  if (c && c.pending > 0)
    return { icon: sv.error ? "off" : "up", text: `${c.pending} kayıt telefonda saklı · ${sv.error ? "internet gelince" : "birazdan"} gönderilecek` };
  if (c) return { icon: "ok", text: sv.lastOk ? `Tüm kayıtlar gönderildi · ${sv.lastOk.slice(0, 5)}` : "Tüm kayıtlar gönderildi" };
  return { icon: null, text: "" };
}

function StatusPill({ s, onOpen }: { s: Snapshot; onOpen: () => void }) {
  const l = linkWords(s);
  const y = syncWords(s);
  return (
    <button className="pill" onClick={onOpen} aria-label="Bağlantı ve sunucu">
      <span className={"dot " + l.dot} />
      <span className="pill-text">{l.text}</span>
      {y.icon === "ok" && <CloudCheck size={18} className="soft" />}
      {y.icon === "up" && <CloudArrowUp size={18} className="warn" />}
      {y.icon === "off" && <CloudSlash size={18} className="soft" />}
    </button>
  );
}

// ---------------------------------------------------------------- herd list --
function HerdRow({ a, onOpen }: { a: Animal; onOpen: (id: string) => void }) {
  return (
    <button className="row" onClick={() => onOpen(a.id)}>
      <Avatar a={a} />
      <span className="row-main">
        <span className="row-title">{a.name}</span>
        <span className={"row-sub " + a.freshness}>
          {a.seen}{a.movement ? ` · ${a.movement}` : ""}
        </span>
      </span>
      <BatteryIcon a={a} />
      <CaretRight size={18} className="soft" />
    </button>
  );
}

function EmptyHerd({ onConnect }: { onConnect: () => void }) {
  return (
    <div className="empty">
      <Cow size={44} className="soft" />
      <p>Henüz konum yok. İstasyona bağlanınca hayvanlarınız burada görünür.</p>
      <button className="primary" onClick={onConnect}>İstasyona bağlan</button>
    </div>
  );
}

// -------------------------------------------------------------- animal card --
function AnimalCard({ a, me, now, onClose, onEdit, onLocate }: {
  a: Animal; me: MyPosition | null; now: number;
  onClose: () => void; onEdit: () => void; onLocate: () => void;
}) {
  const p = a.last;
  let where: string | null = null;
  if (p && me) {
    const d = distanceM(me.lat, me.lon, p.lat, p.lon);
    where = d < 25 ? "Hemen yanınızda" : `${distanceText(d)} · ${directionName(bearingDeg(me.lat, me.lon, p.lat, p.lon))}`;
  }
  // Capacitor hands non-web URLs to Android: geo: opens the phone's maps app.
  const open = () => { if (p) window.location.href = `geo:${p.lat},${p.lon}?q=${p.lat},${p.lon}(${encodeURIComponent(a.name)})`; };
  void now;
  return (
    <div className="card-animal">
      <div className="ca-head">
        <Avatar a={a} size={52} />
        <div className="ca-title">
          <h2>{a.name}</h2>
          <span className={"ca-seen " + a.freshness}>{a.seen}</span>
        </div>
        <button className="icon" onClick={onClose} aria-label="Kapat"><X size={22} /></button>
      </div>
      <div className="facts">
        {p && (where
          ? <div className="fact"><NavigationArrow size={20} weight="fill" className="accent"
              style={{ transform: `rotate(${me ? bearingDeg(me.lat, me.lon, p.lat, p.lon) - 45 : 0}deg)` }} />
              <span>{where}</span></div>
          : <button className="fact link" onClick={onLocate}><NavigationArrow size={20} className="accent" />
              <span>Bana uzaklığını göster</span></button>)}
        {a.movement && <div className="fact"><Cow size={20} className="soft" /><span>Şu an {a.movement}</span></div>}
        {a.batteryText && <div className="fact"><BatteryIcon a={a} /><span>{a.batteryText}</span></div>}
        {a.trail.length > 1 && <div className="fact"><span className="trail-swatch" style={{ background: a.color }} />
          <span>Son 24 saatin izi haritada</span></div>}
      </div>
      <div className="ca-actions">
        {p && <button className="primary" onClick={open}><ArrowSquareOut size={20} /> Yol tarifi</button>}
        <button onClick={onEdit}><PencilSimple size={20} /> Adını değiştir</button>
      </div>
      {p && (
        <p className="fine">
          Son konum {a.lastTime ? clock(a.lastTime) : "?"} · {p.lat.toFixed(5)}, {p.lon.toFixed(5)}
          {p.sats ? ` · ${p.sats} uydu` : ""} · tasma {shortId(a.id)}
        </p>
      )}
    </div>
  );
}

// ------------------------------------------------------------------ modals --
function Modal({ title, onClose, children }: { title: string; onClose: () => void; children: ReactNode }) {
  return (
    <div className="modal-back" onClick={onClose}>
      <div className="modal" role="dialog" aria-label={title} onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h2>{title}</h2>
          <button className="icon" onClick={onClose} aria-label="Kapat"><X size={22} /></button>
        </div>
        <div className="modal-body">{children}</div>
      </div>
    </div>
  );
}

const SWATCHES = ["#3E6B3A", "#A8552E", "#2F6F8F", "#8A6D1F", "#6B4E8A", "#2E7D6B", "#9A4F6A", "#5B6B2E"];

function EditAnimal({ a, onClose }: { a: Animal; onClose: () => void }) {
  const [name, setName] = useState(a.name.startsWith("Tasma ") ? "" : a.name);
  const [color, setColor] = useState(a.color);
  const [note, setNote] = useState(profiles.all[a.id]?.note ?? "");
  const save = () => { profiles.set(a.id, { name, color, note }); onClose(); };
  return (
    <Modal title="Hayvan bilgileri" onClose={onClose}>
      <label className="field">Adı
        <input value={name} placeholder={`Örn. Sarıkız (tasma ${shortId(a.id)})`} onChange={(e) => setName(e.target.value)} autoFocus />
      </label>
      <div className="field">Rengi
        <div className="swatches">
          {SWATCHES.map((c) => (
            <button key={c} className={"swatch" + (c === color ? " on" : "")} style={{ background: c }}
              onClick={() => setColor(c)} aria-label="Renk seç" />
          ))}
        </div>
      </div>
      <label className="field">Not
        <textarea value={note} rows={3} placeholder="Küpe no, yaş, buzağı…" onChange={(e) => setNote(e.target.value)} />
      </label>
      <button className="primary wide" onClick={save}>Kaydet</button>
    </Modal>
  );
}

function AddCollar({ onClose }: { onClose: () => void }) {
  return (
    <Modal title="Yeni tasma ekle" onClose={onClose}>
      <p>Tasmayı uygulamadan kurmak bir sonraki güncellemeyle geliyor: tasmayı USB ile telefona takacak,
        adını verecek ve tamam diyeceksiniz.</p>
      <p className="soft-text">Şimdilik tasma bilgisayardan kurulur. Kurulan tasma, istasyona ilk konumunu
        gönderdiğinde burada kendiliğinden görünür; adını buradan verebilirsiniz.</p>
      <button className="wide" onClick={onClose}>Tamam</button>
    </Modal>
  );
}

function ConnectionPanel({ c, s, onClose }: { c: Controller; s: Snapshot; onClose: () => void }) {
  const sv = s.server;
  const [url, setUrl] = useState(sv.url);
  const [token, setToken] = useState(sv.token);
  const [auto, setAuto] = useState(sv.auto);
  const dirty = url !== sv.url || token !== sv.token || auto !== sv.auto;
  const busy = s.link !== "none";
  const l = linkWords(s), y = syncWords(s);
  const logEnd = useRef<HTMLDivElement>(null);
  const cnt = s.counts;
  return (
    <Modal title="İstasyon ve sunucu" onClose={onClose}>
      <section className="block">
        <div className="block-line"><span className={"dot " + l.dot} /><b>{l.text}</b></div>
        {s.detail && s.state !== "connected" && <p className="soft-text">{s.detail}</p>}
        {s.gatewayHeld && <p className="soft-text">{Number(s.gatewayHeld.split("/")[0])
          ? `İstasyonda telefona aktarılmayı bekleyen ${s.gatewayHeld.split("/")[0]} kayıt var`
          : "İstasyonda bekleyen kayıt yok"}</p>}
        <div className="btn-col">
          {!busy && <>
            <button className="primary" disabled={!s.ready} onClick={() => c.connectBle(false)}>
              <Bluetooth size={20} /> Bluetooth ile bağlan</button>
            <button disabled={!s.ready} onClick={() => c.connectUsb()}><Usb size={20} /> USB kablosuyla bağlan</button>
            <button className="quiet" disabled={!s.ready} onClick={() => c.connectBle(true)}>Başka istasyon seç</button>
          </>}
          {busy && <button className="danger" onClick={() => c.disconnect()}>Bağlantıyı kes</button>}
        </div>
        <p className="soft-text">Bağlıyken ekran kapansa da kayıtlar gelmeye devam eder. İstasyon, bir kaydı ancak
          telefon onu kalıcı yazdıktan sonra siler.</p>
      </section>

      <section className="block">
        <div className="block-line"><b>Sunucu</b></div>
        <p className="soft-text">{y.text}</p>
        <label className="field">Adres
          <input type="url" inputMode="url" autoCapitalize="off" autoCorrect="off"
            placeholder="http://100.114.36.9:8090" value={url} onChange={(e) => setUrl(e.target.value)} />
        </label>
        <label className="field">Anahtar
          <input type="password" autoCapitalize="off" autoCorrect="off" value={token} onChange={(e) => setToken(e.target.value)} />
        </label>
        <label className="check"><input type="checkbox" checked={auto} onChange={(e) => setAuto(e.target.checked)} />
          Kendiliğinden gönder</label>
        <div className="btn-row">
          <button disabled={!dirty} onClick={() => c.saveServer(url, token, auto)}>Kaydet</button>
          <button className="primary" disabled={!sv.url || !sv.token || sv.busy} onClick={() => c.sync(true)}>
            {sv.busy ? "Gönderiliyor…" : "Şimdi gönder"}</button>
        </div>
        {sv.error && <p className="soft-text warn">Son deneme: {sv.error}</p>}
      </section>

      <details className="block tech" onToggle={() => logEnd.current?.scrollIntoView({ block: "end" })}>
        <summary>Teknik ayrıntılar</summary>
        <table className="kv"><tbody>
          <tr><td>İstasyon kimliği</td><td><code>{s.gateway ?? "—"}</code></td></tr>
          <tr><td>İstasyonun tuttuğu</td><td>{s.gatewayHeld ?? "—"}</td></tr>
          <tr><td>Bu oturumda yeni / vardı</td><td>{cnt.STORED} / {cnt.DUPLICATE}</td></tr>
          <tr><td>Teslim / bekleyen teslim</td><td>{s.handedOff} / {s.pending}</td></tr>
          <tr><td>Red / çakışma / belirsiz</td><td>{cnt.REJECTED + s.rejected} / {cnt.CONFLICT} / {cnt.UNCERTAIN}</td></tr>
          <tr><td>Telefondaki kayıt</td><td>{s.records ?? "—"}</td></tr>
          {sv.counts && <tr><td>Sunucuda / bekleyen</td><td>{sv.counts.uploaded} / {sv.counts.pending}</td></tr>}
          <tr><td>Depolama</td><td>{s.storage || "—"}</td></tr>
        </tbody></table>
        <div className="log" role="log">
          {s.log.map((line, i) => <div key={i}>{line}</div>)}
          <div ref={logEnd} />
        </div>
      </details>
    </Modal>
  );
}

// --------------------------------------------------------------- pages --
function SuruPage({ herd, onOpen, onAdd, onConnect }: {
  herd: Animal[]; onOpen: (id: string) => void; onAdd: () => void; onConnect: () => void;
}) {
  return (
    <div className="page">
      <header className="page-head">
        <h1>Sürü</h1>
        <button className="primary small" onClick={onAdd}><Plus size={18} /> Yeni tasma</button>
      </header>
      <p className="page-sub">{herdSummary(herd)}</p>
      {herd.length === 0 ? <EmptyHerd onConnect={onConnect} /> :
        <div className="list">{herd.map((a) => <HerdRow key={a.id} a={a} onOpen={onOpen} />)}</div>}
    </div>
  );
}

function dayLabel(ms: number, now: number): string {
  const d = new Date(ms), t = new Date(now), y = new Date(now - 86400000);
  const same = (a: Date, b: Date) => a.toDateString() === b.toDateString();
  if (same(d, t)) return "Bugün";
  if (same(d, y)) return "Dün";
  return d.toLocaleDateString("tr-TR", { day: "numeric", month: "long" });
}

function JournalIcon({ e }: { e: JournalEntry }) {
  switch (e.kind) {
    case "records": return <Cow size={20} className="accent" />;
    case "handoff": return <CheckCircle size={20} className="ok" />;
    case "upload": return <CloudCheck size={20} className="ok" />;
    case "keys": return <CheckCircle size={20} className="soft" />;
    case "warn": return <Warning size={20} className="warn" />;
    default: return <Bluetooth size={20} className="soft" />;
  }
}

function DefterPage({ s, now, onOpenLink }: { s: Snapshot; now: number; onOpenLink: () => void }) {
  const y = syncWords(s);
  const groups: { label: string; items: JournalEntry[] }[] = [];
  for (const e of [...s.journal].reverse()) {
    const label = dayLabel(e.at, now);
    if (!groups.length || groups[groups.length - 1].label !== label) groups.push({ label, items: [] });
    groups[groups.length - 1].items.push(e);
  }
  return (
    <div className="page">
      <header className="page-head"><h1>Defter</h1></header>
      <button className="summary-card" onClick={onOpenLink}>
        {y.icon === "ok" ? <CloudCheck size={26} className="ok" /> : y.icon === "off" ? <CloudSlash size={26} className="soft" />
          : <CloudArrowUp size={26} className="warn" />}
        <span>
          <b>{y.text || "Kayıtlar telefonda"}</b>
          <span className="soft-text">Telefonda {s.records ?? 0} kayıt var · hiçbiri silinmez</span>
        </span>
        <CaretRight size={18} className="soft" />
      </button>
      {groups.length === 0 && <p className="page-sub">Bağlantı ve gönderimler burada sırayla görünür.</p>}
      {groups.map((g) => (
        <section key={g.label}>
          <h3 className="day">{g.label}</h3>
          <div className="timeline">
            {g.items.map((e, i) => (
              <div key={i} className="tl-item">
                <JournalIcon e={e} />
                <span className="tl-text">{e.text}</span>
                <span className="tl-time">{clock(e.at)}</span>
              </div>
            ))}
          </div>
        </section>
      ))}
    </div>
  );
}

// ------------------------------------------------------------------- app --
export function App({ controller: c }: { controller: Controller }) {
  const s = useSnap(c);
  const prof = useSyncExternalStore((fn) => profiles.subscribe(fn), () => profiles.all);
  const points = usePoints(c, s.pointsVersion);
  const now = useNow(30000);
  const herd = useMemo(() => buildHerd(points?.points ?? [], prof, now), [points, prof, now]);

  const [tab, setTab] = useState<Tab>("harita");
  const [snap, setSnapState] = useState<Snap>("peek");
  const [selected, setSelected] = useState<string | null>(null);
  const [panel, setPanel] = useState<null | "link" | "add" | "edit">(null);
  const [layer, setLayerState] = useState<Layer>(loadLayer);
  const [layerMenu, setLayerMenu] = useState(false);
  const [me, setMe] = useState<MyPosition | null>(null);
  const [sheetPx, setSheetPx] = useState(128);
  const [fitKey, setFitKey] = useState(0);
  const [meKey, setMeKey] = useState(0);
  const watch = useRef<number | null>(null);

  const animal = herd.find((a) => a.id === selected) ?? null;
  const setLayer = (l: Layer) => { setLayerState(l); setLayerMenu(false); try { localStorage.setItem(LAYER_KEY, l); } catch { /* ok */ } };

  const locate = useCallback((center: boolean) => {
    if (center) setMeKey((k) => k + 1);
    if (watch.current !== null || !navigator.geolocation) return;
    watch.current = navigator.geolocation.watchPosition(
      (p) => setMe({ lat: p.coords.latitude, lon: p.coords.longitude, accuracy: p.coords.accuracy }),
      () => { if (watch.current !== null) navigator.geolocation.clearWatch(watch.current); watch.current = null; },
      { enableHighAccuracy: true, maximumAge: 10000 });
  }, []);
  useEffect(() => () => { if (watch.current !== null) navigator.geolocation.clearWatch(watch.current); }, []);

  const openAnimal = (id: string) => { setSelected(id); setTab("harita"); setSnapState("half"); };

  // Android back: close what is open, step by step; then to the background.
  useEffect(() => {
    const h = CapApp.addListener("backButton", () => {
      if (layerMenu) setLayerMenu(false);
      else if (panel) setPanel(null);
      else if (selected) setSelected(null);
      else if (tab !== "harita") setTab("harita");
      else if (snap !== "peek") setSnapState("peek");
      else void CapApp.minimizeApp();
    });
    return () => { void h.then((x) => x.remove()); };
  }, [layerMenu, panel, selected, tab, snap]);

  const summary = herdSummary(herd);
  const attention = herd.filter((a) => a.attention >= 20).length;

  return (
    <div className="shell">
      <main className="stage">
        <section className="map-page" hidden={tab !== "harita"}>
          <MapView herd={herd} selected={selected} layer={layer} me={me} fitKey={fitKey}
            centerMeKey={meKey} bottomInset={sheetPx} onSelect={(id) => { setSelected(id); if (id) setSnapState("half"); }} />
          <div className="map-top">
            <StatusPill s={s} onOpen={() => setPanel("link")} />
          </div>
          <div className="map-tools">
            <button className="fab" onClick={() => setLayerMenu((v) => !v)} aria-label="Harita türü"><Stack size={24} /></button>
            <button className="fab" onClick={() => locate(true)} aria-label="Konumum"><Crosshair size={24} /></button>
            <button className="fab" onClick={() => setFitKey((k) => k + 1)} aria-label="Bütün sürüyü göster"><CornersOut size={24} /></button>
            {layerMenu && (
              <div className="layer-menu">
                {(Object.keys(LAYER_NAMES) as Layer[]).map((l) => (
                  <button key={l} className={l === layer ? "on" : ""} onClick={() => setLayer(l)}>{LAYER_NAMES[l]}</button>
                ))}
                <p className="fine">Harita görüntüsü internetle gelir; hayvanların yeri internetsiz de görünür.</p>
              </div>
            )}
          </div>
          <Sheet snap={snap} onSnap={setSnapState} onHeight={setSheetPx}
            header={animal ? null : (
              <div className="sheet-summary">
                <span className="sum-text">{summary}</span>
                {attention > 0 && <span className="badge warn">{attention}</span>}
              </div>
            )}>
            {animal
              ? <AnimalCard a={animal} me={me} now={now} onClose={() => { setSelected(null); setSnapState("peek"); }}
                  onEdit={() => setPanel("edit")} onLocate={() => locate(false)} />
              : herd.length === 0
                ? <EmptyHerd onConnect={() => setPanel("link")} />
                : <div className="list">{herd.map((a) => <HerdRow key={a.id} a={a} onOpen={openAnimal} />)}</div>}
          </Sheet>
        </section>
        {tab === "suru" && <SuruPage herd={herd} onOpen={openAnimal} onAdd={() => setPanel("add")} onConnect={() => setPanel("link")} />}
        {tab === "defter" && <DefterPage s={s} now={now} onOpenLink={() => setPanel("link")} />}
      </main>

      <nav className="tabbar" role="tablist">
        <button role="tab" aria-selected={tab === "harita"} onClick={() => setTab("harita")}>
          <MapTrifold size={26} weight={tab === "harita" ? "fill" : "regular"} /><span>Harita</span></button>
        <button role="tab" aria-selected={tab === "suru"} onClick={() => setTab("suru")}>
          <span className="tab-icon"><Cow size={26} weight={tab === "suru" ? "fill" : "regular"} />
            {attention > 0 && <span className="tab-badge" />}</span><span>Sürü</span></button>
        <button role="tab" aria-selected={tab === "defter"} onClick={() => setTab("defter")}>
          <Notebook size={26} weight={tab === "defter" ? "fill" : "regular"} /><span>Defter</span></button>
      </nav>

      {panel === "link" && <ConnectionPanel c={c} s={s} onClose={() => setPanel(null)} />}
      {panel === "add" && <AddCollar onClose={() => setPanel(null)} />}
      {panel === "edit" && animal && <EditAnimal a={animal} onClose={() => setPanel(null)} />}
      {s.error && <div className="toast bad">Telefon kayıt defteri açılamadı: {s.error}</div>}
    </div>
  );
}
