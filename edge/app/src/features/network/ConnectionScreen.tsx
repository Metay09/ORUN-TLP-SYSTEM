// Bağlantı (Meshtastic düzeni, ORUN'a göre): üstte sunucu satırı; "Bu telefon"
// kartı (bağlı cihaz ya da "Cihaz seçilmedi"); Bluetooth / USB seçici ve
// yakındaki ORUN cihazları; çiftliğin diğer gateway'leri (nasıl bağlı, son
// görülme, bellek, kaç tasma); en altta teknik ayrıntılar.
import { Fragment, useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";
import {
  Bluetooth, CaretRight, CellSignalFull, CloudCheck, DeviceMobile, MagnifyingGlass, MapPin, Question, Usb, WifiHigh, WifiSlash, X,
} from "@phosphor-icons/react";
import { probeUsbDevice } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import type { FarmGateway } from "../../app/ownerAccountClient.ts";
import {
  deviceNames, onPilotAccountChange, onPilotObservationSync, pilotAccountStatus, pilotGateways, pilotObservationSyncState,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Bar, Card, Cell, DemoFlag, Dialog, Header, KV, Section, Seg } from "../../ds/components.tsx";
import { classifyNearby, mergeNearby, scanNearby, stopNearbyScan, type NearbyDevice, type NearbyView } from "../../platform/bleScan.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { AddTrackerDialog } from "../herd/AddTrackerDialog.tsx";
import { startPosition, usePosition } from "../map/position.ts";
import { NameDialog } from "./NameDialog.tsx";
import { farmChain, gatewayFacts, gatewayLine, stationWords, type ChainNode } from "./words.ts";

type Via = "ble" | "usb";

const ASKED = "orun.namePrompted.v1";
function asked(): string[] {
  try { const v = JSON.parse(localStorage.getItem(ASKED) ?? "[]"); return Array.isArray(v) ? v : []; } catch { return []; }
}
function markAsked(id: string): void {
  try { localStorage.setItem(ASKED, JSON.stringify([...asked(), id.toUpperCase()].slice(-200))); } catch { /* fine */ }
}

export function ConnectionScreen() {
  const m = useApp();
  const nav = useNav();
  const pos = usePosition();
  const st = m.station, s = m.snap, sv = m.server;
  const words = stationWords(st, s);
  const linkOn = st.link !== "none";
  const held = st.held ?? 0;
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const [sync, setSync] = useState(pilotObservationSyncState);
  const [via, setVia] = useState<Via>("ble");
  const [marking, setMarking] = useState(false);
  const [gateways, setGateways] = useState<FarmGateway[] | null>(null);
  const [nearby, setNearby] = useState<NearbyDevice[]>([]);
  const [scanning, setScanning] = useState(false);
  const [note, setNote] = useState("");
  const [usbBusy, setUsbBusy] = useState(false);
  const [usbTracker, setUsbTracker] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);
  const [naming, setNaming] = useState<string | null>(null);
  const [node, setNode] = useState<ChainNode["key"] | null>(null);
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const label = (id: string) => names[id.toUpperCase()] ?? bleName(id);
  useEffect(() => onPilotObservationSync(() => setSync(pilotObservationSyncState())), []);
  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  // Çiftliğin gateway'leri: açılınca ve dakikada bir.
  useEffect(() => {
    let live = true;
    const load = () => { pilotGateways().then((g) => { if (live) setGateways(g); }).catch(() => undefined); };
    load();
    const t = setInterval(load, 60_000);
    return () => { live = false; clearInterval(t); };
  }, [account?.username]);

  const scan = useCallback(async () => {
    if (scanning) return;
    setScanning(true); setNote(""); setNearby([]);
    try { await scanNearby((d) => setNearby((list) => mergeNearby(list, d))); }
    catch (e) { setNote("Bluetooth taraması yapılamadı: " + (e instanceof Error ? e.message : String(e))); }
    finally { setScanning(false); }
  }, [scanning]);
  useEffect(() => () => { void stopNearbyScan(); }, []);

  const known = (gateways ?? []).map((g) => g.gateway);
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));
  const views: NearbyView[] = nearby.map((d) => classifyNearby(d, s.gateway ? [...known, s.gateway] : known, trackers, label));

  // 2d: adı olmayan bir gateway ilk görüldüğünde ad sorulur (bir kez).
  const unnamed = [
    ...(linkOn && st.phase === "connected" && st.id ? [st.id] : []),
    ...(gateways ?? []).map((g) => g.gateway),
  ].find((id) => !names[id.toUpperCase()] && !asked().includes(id.toUpperCase()));
  useEffect(() => {
    if (!m.demo && account && unnamed && !naming) setNaming(unnamed);
  }, [unnamed, account, m.demo]); // eslint-disable-line react-hooks/exhaustive-deps

  const pick = (d: NearbyView) => {
    if (d.kind === "tracker") {
      setNote(`${d.label}: tasmaya Bluetooth ile bağlanma bir sonraki güncellemede geliyor. Şimdilik USB kablosuyla bağlanabilirsin.`);
      return;
    }
    void stopNearbyScan();
    void m.source.connectBle(false, d.deviceId);
  };

  async function usbConnect(): Promise<void> {
    setUsbBusy(true); setNote("");
    let con: UsbConsole | null = null;
    try {
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      await con.close(); con = null;
      if (dev.kind === "gateway") await m.source.connectUsb();
      else setUsbTracker(dev.report.device);
    } catch (e) {
      setNote(e instanceof Error && e.message.length < 200 ? e.message : "USB ile bağlanılamadı.");
    } finally {
      if (con) await con.close();
      setUsbBusy(false);
    }
  }

  const others = (gateways ?? []).filter((g) => !(linkOn && s.gateway && g.gateway.toUpperCase() === s.gateway.toUpperCase()));
  const chain = farmChain({
    trackers: m.herd.filter((a) => a.last).length,
    fresh: m.herd.filter((a) => a.freshness === "fresh").length,
    gateways: account ? gateways : null,
    phoneGateway: linkOn && st.phase === "connected" && st.id ? { id: st.id, link: st.link as "ble" | "usb" } : null,
    signedIn: Boolean(account), sync, pending: sv.pending, now: m.now, label,
  });
  const picked = chain.nodes.find((n) => n.key === node) ?? null;
  const ICON = { tasmalar: <CellSignalFull size={22} />, gateway: <WifiHigh size={22} />, sunucu: <CloudCheck size={22} />, telefon: <DeviceMobile size={22} /> };
  return (
    <div className="page">
      <Header title={tr.network.title} />
      {m.demo && <DemoFlag onPage />}

      <Section title={tr.network.chain}
        info="Kayıt tasmadan gateway'e, oradan sunucuya, sunucudan telefonlara gider. Bir kayıt ancak sonraki halka onu kalıcı yazdıktan sonra öncekinden silinir; bağlantı koparsa kayıt beklediği yerde durur, kaybolmaz. Bir halkaya dokununca ayrıntısı görünür." />
      <Card>
        <div className="chain">
          {chain.nodes.map((n, idx) => (
            <Fragment key={n.key}>
              {idx > 0 && <span className={"wire " + chain.wires[idx - 1]} />}
              <button className="node" type="button" aria-pressed={node === n.key}
                onClick={() => setNode(node === n.key ? null : n.key)}>
                <span className={"nd ic " + n.tone}>{ICON[n.key]}</span>
                <b>{n.title}</b>
                <span>{n.sub}</span>
              </button>
            </Fragment>
          ))}
        </div>
        {picked && (
          <p className="sm chain-detail">
            {picked.detail}
            {picked.key === "telefon" && !account && <> · <button className="lnk" onClick={() => nav.push({ to: "settings" })}>Giriş yap</button></>}
            {picked.key === "tasmalar" && m.herd.length > 0 && <> · <button className="lnk" onClick={() => nav.setTab("herd")}>Sürü</button></>}
          </p>
        )}
      </Card>

      <Section title="Bu telefon" info={tr.station.background} />
      <Card>
        {linkOn ? (
          <div className="stack" style={{ padding: 16 }}>
            <div className="row" style={{ gap: 12 }}>
              <span className={"ic " + (words.dot === "ok" ? "fresh" : "aging")}><WifiHigh size={22} /></span>
              <div className="stack" style={{ gap: 2 }}>
                <span className="strong">{st.id ? label(st.id) : "Gateway"}</span>
                <span className="sm">{st.detail && st.phase !== "connected" ? `${words.text} · ${st.detail}` : words.text}</span>
              </div>
            </div>
            {st.id && account && !m.demo && (
              <button className="btn quiet small" style={{ alignSelf: "flex-start" }} onClick={() => setNaming(st.id)}>
                {names[st.id.toUpperCase()] ? "Adını değiştir" : "Ad ver"}
              </button>
            )}
            {st.capacity !== null && (
              <>
                <div className="row" style={{ justifyContent: "space-between" }}><span className="sm">{tr.station.memory}</span><span className="sm strong">{fmt.num(held)} / {fmt.num(st.capacity)}</span></div>
                <Bar ratio={held / st.capacity} tone={held / st.capacity >= 0.8 ? "bad" : held / st.capacity >= 0.5 ? "warn" : undefined} />
              </>
            )}
            {st.id && (
              <>
                <div className="row" style={{ justifyContent: "space-between" }}>
                  <span className="sm">{tr.station.position}</span>
                  <span className="sm strong">{st.position ? fmt.coords(st.position.lat, st.position.lon) : tr.station.positionNone}</span>
                </div>
                <button className="btn wide" onClick={() => { startPosition(); setMarking(true); }} disabled={marking}>
                  <MapPin size={20} />{tr.station.markHere}
                </button>
                {st.position && <button className="btn quiet wide" onClick={() => m.actions.markStation(null)}><X size={18} />{tr.station.clearPosition}</button>}
                {marking && !pos.pos && <span className="fine">{tr.find.noPhone}</span>}
              </>
            )}
            <button className="btn quiet small" style={{ color: "var(--alarm-t)", alignSelf: "flex-start" }} onClick={() => m.source.disconnect()}>{tr.station.disconnect}</button>
          </div>
        ) : (
          <div className="stack" style={{ padding: "22px 16px", alignItems: "center", textAlign: "center" }}>
            <span className="ic"><WifiSlash size={26} /></span>
            <span className="sm">Cihaz seçilmedi</span>
          </div>
        )}
      </Card>

      {!linkOn && (
        <>
          <div style={{ padding: "12px 16px 0" }}>
            <Seg<Via> value={via} onChange={setVia}
              options={[{ value: "ble", label: tr.station.ble }, { value: "usb", label: tr.station.usb }]} />
          </div>
          {via === "ble" ? (
            <>
              <Section title="Yakındaki cihazlar" action={scanning ? "Taranıyor…" : "Tara"} onAction={() => { void scan(); }} />
              <Card>
                {views.length ? views.map((d) => (
                  <Cell key={d.deviceId}
                    lead={<span className={"ic " + (d.kind === "gateway" ? "fresh" : "")}>
                      {d.kind === "gateway" ? <WifiHigh size={20} /> : d.kind === "tracker" ? <CellSignalFull size={20} /> : <Question size={20} />}
                    </span>}
                    title={d.label}
                    sub={[d.kind === "gateway" ? "Gateway" : d.kind === "tracker" ? "Tasma" : "Tanınmayan ORUN cihazı",
                          d.rssi !== null ? `${d.rssi} dBm` : ""].filter(Boolean).join(" · ")}
                    trail={<CaretRight size={18} color="var(--mute)" />}
                    onClick={() => pick(d)} />
                )) : (
                  <div className="stack" style={{ padding: "18px 16px", alignItems: "center", textAlign: "center" }}>
                    <Bluetooth size={24} />
                    <span className="sm">{scanning ? "Yakındaki ORUN cihazları aranıyor…" : "Henüz cihaz görülmedi"}</span>
                    {!scanning && <button className="btn small" onClick={() => { void scan(); }}><MagnifyingGlass size={18} />Tara</button>}
                  </div>
                )}
              </Card>
              <div style={{ padding: "6px 16px 0" }}>
                <button className="btn quiet small" disabled={!s.ready} onClick={() => m.source.connectBle(true)}>Android listesinden seç</button>
              </div>
            </>
          ) : (
            <>
              <Section title="USB" />
              <Card pad>
                <div className="stack">
                  <span className="sm">Tasmayı ya da gateway'i USB (OTG) kablosuyla telefona bağla.</span>
                  <button className="btn primary wide" disabled={!s.ready || usbBusy} onClick={() => { void usbConnect(); }}>
                    <Usb size={20} />{usbBusy ? "Cihaz okunuyor…" : "USB ile bağlı cihaza bağlan"}
                  </button>
                </div>
              </Card>
            </>
          )}
          {note && <p className="sm lead" role="status">{note}</p>}
        </>
      )}

      <Section title="Diğer gateway'ler" />
      <Card>
        {!account ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Hesaba giriş yapınca çiftliğin gateway'leri burada görünür.</p>
          : gateways === null ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Okunuyor…</p>
          : others.length === 0 ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Başka gateway yok.</p>
          : others.map((g) => (
            <Cell key={g.gateway}
              lead={<span className={"ic " + (g.via ? "fresh" : "")}>{g.via ? <WifiHigh size={20} /> : <WifiSlash size={20} />}</span>}
              title={label(g.gateway)} wrapSub
              sub={<>{gatewayLine(g, m.now)}<br />{gatewayFacts(g)}</>}
              trail={<span className="sm" style={{ color: "var(--brand-t)" }}>{names[g.gateway] ? "" : "Ad ver"}<CaretRight size={18} color="var(--mute)" /></span>}
              onClick={() => setNaming(g.gateway)} />
          ))}
      </Card>

      <TechDetails />

      {usbTracker && (
        <UsbTrackerDialog device={usbTracker} onClose={() => setUsbTracker(null)}
          onAdd={() => { setUsbTracker(null); setAdding(true); }} />
      )}
      {adding && <AddTrackerDialog onClose={() => setAdding(false)} />}
      {naming && (
        <NameDialog device={naming} kind="gateway" current={names[naming.toUpperCase()] ?? null}
          onClose={() => { markAsked(naming); setNaming(null); }} />
      )}
    </div>
  );
}

/** USB ile bağlanan tasmanın kartı: hangi hayvan, pil, son görülme; hesapta yoksa ekle. */
function UsbTrackerDialog({ device, onClose, onAdd }: { device: string; onClose: () => void; onAdd: () => void }) {
  const m = useApp();
  const nav = useNav();
  const a = m.herd.find((x) => x.device && x.device.toUpperCase() === device.toUpperCase()) ?? null;
  return (
    <Dialog title={a ? a.name : bleName(device)} onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <KV k="Bluetooth adı">{bleName(device)}</KV>
        {a ? (
          <>
            <KV k="Son görülme">{a.seen}</KV>
            <KV k={tr.animal.battery}>{a.batteryPercent !== null ? `%${a.batteryPercent}` : "—"}</KV>
            <button className="btn primary wide" onClick={() => { onClose(); nav.push({ to: "animal", id: a.id }); }}>Hayvanın kartını aç</button>
          </>
        ) : (
          <>
            <p className="sm" style={{ margin: 0 }}>Bu tasma hesabında görünmüyor.</p>
            <button className="btn primary wide" onClick={onAdd}>Hesabına ekle</button>
          </>
        )}
      </div>
    </Dialog>
  );
}

function TechDetails() {
  const m = useApp();
  const s = m.snap, c = s.counts, sv = m.server;
  const end = useRef<HTMLDivElement>(null);
  return (
    <details className="tech" onToggle={() => end.current?.scrollIntoView({ block: "end" })}>
      <summary>{tr.common.tech}</summary>
      <Card>
        <KV k={tr.tech.gateway}><code>{s.gateway ?? "—"}</code></KV>
        <KV k={tr.tech.held}>{s.gatewayHeld ?? "—"}</KV>
        <KV k={tr.tech.session}>{c.STORED} / {c.DUPLICATE}</KV>
        <KV k={tr.tech.handoff}>{s.handedOff} / {s.pending}</KV>
        <KV k={tr.tech.rejected}>{c.REJECTED + s.rejected} / {c.CONFLICT} / {c.UNCERTAIN}</KV>
        <KV k={tr.tech.phone}>{s.records ?? "—"}</KV>
        {sv.uploaded !== null && <KV k={tr.tech.server}>{sv.uploaded} / {sv.pending}</KV>}
        {sv.lastOk && <KV k="Son sunucu erişimi">{sv.lastOk}</KV>}
        <KV k={tr.tech.storage}>{s.storage || "—"}</KV>
      </Card>
      <Section title={tr.tech.log} />
      <div className="log" role="log">{s.log.join("\n")}<div ref={end} /></div>
    </details>
  );
}
