// Cihazlar (4b, sahip kararı 2026-11-02; eski adı Bağlantı): üstte "Kayıt
// güvende mi?" ve "Bu telefon" kartı; Bluetooth / USB seçici ve yakındaki ORUN
// cihazları; sonra bütün cihazlar türüne göre (Gateway'ler, Tasmalar; ileride
// Sensörler, Vanalar), her biri kendi ekranına açılır; gruplar; teknik ayrıntılar.
import { Fragment, useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";
import {
  Bluetooth, CaretRight, CellSignalFull, CloudCheck, DeviceMobile, MagnifyingGlass, MapPin, Question, Usb, WifiHigh, WifiSlash, X,
} from "@phosphor-icons/react";
import { noRoleMessage, probeUsbDevice, setRoleAndReconnect, type DeviceRole, type GnssHint } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import type { FarmGateway, EdgeQueue } from "../../app/ownerAccountClient.ts";
import { collarStorageReadings } from "../../app/collarStorageReadings.ts";
import {
  deviceNames, onPilotAccountChange, onPilotObservationSync, pilotAccountStatus, pilotGateways, pilotEdgeQueues, pilotObservationSyncState,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Bar, Card, Cell, Header, KV, Section, Seg } from "../../ds/components.tsx";
import { classifyNearby, mergeNearby, scanNearby, stopNearbyScan, type NearbyDevice, type NearbyView } from "../../platform/bleScan.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { AddTrackerDialog } from "../herd/AddTrackerDialog.tsx";
import { TrackerLiveDialog, type LiveTarget } from "../herd/TrackerLive.tsx";
import { startPosition, usePosition } from "../map/position.ts";
import { NameDialog } from "./NameDialog.tsx";
import { RoleChoice } from "../devices/RoleChoice.tsx";
import { pendingKeys } from "../../app/deviceSettings.ts";
import { BulkSettingsDialog } from "../devices/BulkSettings.tsx";
import { useDeviceSettings } from "../devices/DeviceScreen.tsx";
import { GroupsSection } from "../devices/GroupsSection.tsx";
import { OpDialog, useOps } from "../devices/Ops.tsx";
import { groupLine, groupOps, KIND_WORD } from "../../app/deviceOps.ts";
import { useTrackerFences } from "../zones/ApplyFence.tsx";
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
  const [edgeQueues, setEdgeQueues] = useState<EdgeQueue[] | null>(null);
  const [nearby, setNearby] = useState<NearbyDevice[]>([]);
  const [scanning, setScanning] = useState(false);
  const [note, setNote] = useState("");
  const [usbBusy, setUsbBusy] = useState(false);
  const [noRole, setNoRole] = useState<{ gnss: GnssHint } | null>(null);   // Y2a-2: kurulmamış cihaz
  const [live, setLive] = useState<{ device: string | null; target: LiveTarget; bleId?: string } | null>(null);
  const [adding, setAdding] = useState(false);
  const [naming, setNaming] = useState<string | null>(null);
  const [node, setNode] = useState<ChainNode["key"] | null>(null);
  const [bulk, setBulk] = useState(false);
  const settings = useDeviceSettings();
  const fencePlans = useTrackerFences();
  const opRows = useOps();
  const queued = new Set(opRows.filter((r) => r.kind === "fence" && (r.state === "waiting" || r.state === "gateway")).map((r) => r.device));
  const groups = groupOps(opRows.filter((r) => r.state !== "superseded")).slice(0, 5);
  const [opOpen, setOpOpen] = useState<string | null>(null);
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const collarReadings = useSyncExternalStore(collarStorageReadings.subscribe, collarStorageReadings.snapshot);
  const label = (id: string) => names[id.toUpperCase()] ?? bleName(id);
  useEffect(() => onPilotObservationSync(() => setSync(pilotObservationSyncState())), []);
  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  // Çiftliğin gateway'leri: açılınca ve dakikada bir.
  useEffect(() => {
    let live = true;
    const load = () => {
      pilotGateways().then((g) => { if (live) setGateways(g); })
        .catch(() => { if (live) setGateways(null); });
      pilotEdgeQueues().then((q) => { if (live) setEdgeQueues(q); })
        .catch(() => { if (live) setEdgeQueues(null); });
    };
    load();
    const t = setInterval(load, 60_000);
    return () => { live = false; clearInterval(t); };
  }, [account?.installation]);
  useEffect(() => {
    // A direct BLE/USB observation is local to this phone/account session.
    // Do not leak a prior account's collar memory snapshot after sign-out.
    collarStorageReadings.clear();
    setEdgeQueues(null);
  }, [account?.installation]);

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
    if (account && unnamed && !naming) setNaming(unnamed);
  }, [unnamed, account]); // eslint-disable-line react-hooks/exhaustive-deps

  // 4a/4b: bilinen tasmaya dokununca tasmanın ekranı (durum, bağlan, ayarlar);
  // tanınmayan ORUN cihazına dokununca canlı okuma; gateway'e dokununca köprü.
  const pick = (d: NearbyView) => {
    void stopNearbyScan();
    if (d.kind === "gateway") { void m.source.connectBle(false, d.deviceId); return; }
    if (d.kind === "tracker" && d.id) { nav.push({ to: "device", id: d.id }); return; }
    setLive({ device: d.id, target: { ble: d.deviceId }, bleId: d.deviceId });
  };

  async function usbConnect(role?: DeviceRole): Promise<void> {
    setUsbBusy(true); setNote("");
    let con: UsbConsole | null = null;
    try {
      con = await UsbConsole.open();
      let dev = await probeUsbDevice(con);
      if (role && dev.kind === "unassigned") {
        setNote(`Cihaz ${role === "tasma" ? "tasma" : "gateway"} olarak kuruluyor; yeniden başlıyor… (USB izni sorulursa onayla)`);
        const r = await setRoleAndReconnect(con, role, () => UsbConsole.open());
        con = r.con; dev = r.device;
        setNoRole(null);
        setNote(role === "tasma" ? "Tasma olarak kuruldu; şimdi hesaba ekle." : "Gateway olarak kuruldu; bu telefon köprü olarak bağlanıyor.");
      }
      await con.close(); con = null;
      if (dev.kind === "unassigned") { setNoRole({ gnss: dev.gnss }); return; }
      if (dev.kind === "conflict") { setNote(noRoleMessage("CONFLICT")); return; }
      if (dev.kind === "gateway") await m.source.connectUsb();
      else if (role === "tasma") setAdding(true);
      else setLive({ device: dev.report.device, target: { usb: true } });
    } catch (e) {
      setNote(e instanceof Error && e.message.length < 200 ? e.message : "USB ile bağlanılamadı.");
    } finally {
      if (con) await con.close();
      setUsbBusy(false);
    }
  }

  // 4b: tasmalar türüne göre; önce sorunlular (bekleyen ayar, eski alan, sessiz, pil düşük).
  const trackerRows = m.herd.filter((a) => a.device).map((a) => {
    const id = a.device!, dev = m.devices.find((d) => d.id === id);
    const waiting = pendingKeys(settings[id.toUpperCase()]).length;
    const fence = fencePlans.find((f) => f.device.toUpperCase() === id.toUpperCase());
    const staleFence = fence && (fence.state === "stale" || fence.state === "never");
    const silent = !dev?.lastTime || m.now - dev.lastTime > 3600_000;
    const bits = [dev?.lastTime ? fmt.ago(dev.lastTime, m.now) : "hiç haber yok",
                  dev && dev.batteryPercent !== null ? `%${dev.batteryPercent}` : "",
                  waiting ? `${waiting} ayar sırada` : "", queued.has(id.toUpperCase()) ? "alanlar sırada" : staleFence ? "alanlar değişti" : ""].filter(Boolean);
    const low = dev?.batteryPercent !== null && dev?.batteryPercent !== undefined && dev.batteryPercent < 20;
    return { id, name: a.name, sub: bits.join(" · "), problem: waiting > 0 || !!staleFence || silent || low };
  }).sort((x, y) => Number(y.problem) - Number(x.problem) || x.name.localeCompare(y.name, "tr"));

  const others = (gateways ?? []).filter((g) => !(linkOn && s.gateway && g.gateway.toUpperCase() === s.gateway.toUpperCase()));
  const chain = farmChain({
    trackers: m.herd.filter((a) => a.last).length,
    fresh: m.herd.filter((a) => a.freshness === "fresh").length,
    gateways: account ? gateways : null,
    phoneGateway: linkOn && st.phase === "connected" && st.id ? { id: st.id, link: st.link as "ble" | "usb" } : null,
    signedIn: Boolean(account), sync, pending: sv.pending, now: m.now, label,
  });
  // Freshness applies to the PC Edge heartbeat only; last-known values are
  // still visible per PC when the producer is temporarily offline.
  const connectedEdges = (edgeQueues ?? []).filter((edge) =>
    m.now >= edge.lastSeen && m.now - edge.lastSeen <= 5 * 60_000);
  const pcPending = connectedEdges.length
    ? connectedEdges.reduce((sum, edge) => sum + edge.pending, 0) : null;
  const pcDelivered = connectedEdges.length
    ? connectedEdges.reduce((sum, edge) => sum + edge.delivered, 0) : null;
  const pcQuarantined = connectedEdges.length
    ? connectedEdges.reduce((sum, edge) => sum + edge.quarantined, 0) : null;
  const collarSamples = Object.values(collarReadings).filter((x) =>
    m.herd.some((a) => a.device?.toUpperCase() === x.device));
  const freshCollars = collarSamples.filter((x) =>
    m.now >= x.at && m.now - x.at <= 5 * 60_000);
  const collarStored = freshCollars.length
    ? freshCollars.reduce((sum, x) => sum + x.count, 0) : null;
  const reportedGateways = (gateways ?? []).filter((g) => g.held !== null);
  const gatewayMemory = reportedGateways.length
    ? reportedGateways.reduce((sum, g) => sum + (g.held ?? 0), 0)
    : st.phase === "connected" ? st.held : null;
  const gatewayMemorySources = reportedGateways.length || (st.phase === "connected" && st.held !== null ? 1 : 0);
  const picked = chain.nodes.find((n) => n.key === node) ?? null;
  const ICON = { tasmalar: <CellSignalFull size={22} />, gateway: <WifiHigh size={22} />, sunucu: <CloudCheck size={22} />, telefon: <DeviceMobile size={22} /> };
  return (
    <div className="page">
      <Header title={tr.network.title} />

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
        <div className="stack" style={{ padding: "8px 16px 16px", gap: 10 }}>
          <span className="strong">Kayıt ve kuyruk durumu</span>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">Tasma belleği (doğrudan okuma)</span>
            <span className="sm strong">{collarStored === null ? "— · güncel okuma yok" : `${fmt.num(collarStored)} kayıt · ${freshCollars.length} tasma`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">Gateway belleğinde</span>
            <span className="sm strong">{gatewayMemory === null ? "— · sayaç alınmadı" : `${fmt.num(gatewayMemory)} kayıt · ${gatewayMemorySources} gateway`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">PC Edge'de sunucu bekleyen</span>
            <span className="sm strong">{pcPending === null ? "— · güncel rapor yok" : `${fmt.num(pcPending)} kayıt · ${connectedEdges.length} PC`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">PC Edge sunucuda onaylanan</span>
            <span className="sm strong">{pcDelivered === null ? "— · güncel rapor yok" : `${fmt.num(pcDelivered)} kayıt`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">PC Edge sorunlu/karantinada</span>
            <span className="sm strong">{pcQuarantined === null ? "— · güncel rapor yok" : `${fmt.num(pcQuarantined)} kayıt`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">Bu telefondan sunucu bekleyen</span>
            <span className="sm strong">{sv.pending === null ? "— · sayaç alınmadı" : `${fmt.num(sv.pending)} kayıt`}</span>
          </div>
          <div className="row" style={{ justifyContent: "space-between", gap: 12 }}>
            <span className="sm">Bu telefondan sunucuda onaylanan</span>
            <span className="sm strong">{sv.uploaded === null ? "— · sayaç alınmadı" : `${fmt.num(sv.uploaded)} kayıt`}</span>
          </div>
          <span className="fine">Kayıtlar aşamalar arasında kopyalanmış olabilir; bu sayıları toplayıp benzersiz paket sayısı sayma. Tasma değeri doğrudan APP STORAGE belleğidir, kesin LoRa/ACK kuyruğu değildir. PC Edge değerleri sunucu tarafından onaylanan son defter özetidir. Telefonun onayı çiftlik toplamı değildir.</span>
          {(edgeQueues !== null || collarSamples.length > 0) && (
            <details className="tech">
              <summary>Kaynaklara göre sayaçlar ve son ölçüm</summary>
              {edgeQueues?.map((edge) => (
                <div className="row" key={edge.edge} style={{ justifyContent: "space-between", gap: 8, padding: "8px 16px" }}>
                  <span className="sm">{edge.edge} · {fmt.ago(edge.lastSeen, m.now)}</span>
                  <span className="sm strong">
                    {fmt.num(edge.pending)} bekleyen / {fmt.num(edge.delivered)} onaylı / {fmt.num(edge.quarantined)} sorunlu
                    {m.now - edge.lastSeen > 5 * 60_000 ? " · eski ölçüm" : ""}
                  </span>
                </div>
              ))}
              {collarSamples.map((sample) => (
                <div className="row" key={sample.device} style={{ justifyContent: "space-between", gap: 8, padding: "8px 16px" }}>
                  <span className="sm">{label(sample.device)} · {sample.via.toUpperCase()} · {fmt.ago(sample.at, m.now)}</span>
                  <span className="sm strong">
                    {fmt.num(sample.count)} / {fmt.num(sample.capacity)} bellek
                    {m.now - sample.at > 5 * 60_000 ? " · eski ölçüm" : ""}
                  </span>
                </div>
              ))}
            </details>
          )}
        </div>
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
            {st.id && account && (
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
                  {!noRole && <button className="btn primary wide" disabled={!s.ready || usbBusy} onClick={() => { void usbConnect(); }}>
                    <Usb size={20} />{usbBusy ? "Cihaz okunuyor…" : "USB ile bağlı cihaza bağlan"}
                  </button>}
                  {noRole && <RoleChoice gnss={noRole.gnss} busy={usbBusy} onPick={(r) => { void usbConnect(r); }}
                    onCancel={() => setNoRole(null)} />}
                </div>
              </Card>
            </>
          )}
          {note && <p className="sm lead" role="status">{note}</p>}
        </>
      )}

      <Section title="Gateway'ler" />
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
              onClick={() => nav.push({ to: "device", id: g.gateway })} />
          ))}
      </Card>

      <Section title="Tasmalar" action={trackerRows.length ? "Toplu ayar" : undefined} onAction={() => setBulk(true)}
        info="Her tasmanın ekranında durum, bağlan ve ayarlar. Ayar hesaba kaydedilir, tasmaya bağlanınca sormadan gider. Toplu ayar: hepsine, bir gruba ya da seçtiklerine." />
      <Card>
        {trackerRows.length ? trackerRows.map((r) => (
          <Cell key={r.id}
            lead={<span className={"ic " + (r.problem ? "aging" : "fresh")}><CellSignalFull size={20} /></span>}
            title={r.name} wrapSub sub={r.sub} subTone={r.problem ? "aging" : undefined}
            trail={<CaretRight size={18} color="var(--mute)" />}
            onClick={() => nav.push({ to: "device", id: r.id })} />
        )) : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Henüz tasma yok. Sürü → "+" ile ekle.</p>}
      </Card>

      {groups.length > 0 && <>
        <Section title="İşlemler" info="Ayar kayıtları ve 'Tasmalara uygula'. Sıradakiler tasmaya bağlanınca (şimdilik USB) kendiliğinden işlenir; 'Tasmaya işlendi' yalnız tasma onaylayınca yazılır." />
        <Card>
          {groups.map((g) => (
            <Cell key={g.op} title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`} wrapSub sub={groupLine(g)}
              subTone={g.targets.some((t) => t.state === "rejected") ? "alarm" : g.targets.every((t) => t.state === "applied" || t.state === "same") ? "fresh" : undefined}
              trail={<CaretRight size={18} color="var(--mute)" />} onClick={() => setOpOpen(g.op)} />
          ))}
        </Card>
      </>}
      {opOpen && <OpDialog op={opOpen} onClose={() => setOpOpen(null)} />}

      <GroupsSection />
      {bulk && <BulkSettingsDialog onClose={() => setBulk(false)} />}

      <TechDetails />

      {live && (
        <TrackerLiveDialog device={live.device} target={live.target} onClose={() => setLive(null)}
          onAdd={() => { setLive(null); setAdding(true); }}
          onBridge={live.bleId ? () => { const id = live.bleId!; setLive(null); void m.source.connectBle(false, id); } : undefined} />
      )}
      {adding && <AddTrackerDialog onClose={() => setAdding(false)} />}
      {naming && (
        <NameDialog device={naming} kind="gateway" current={names[naming.toUpperCase()] ?? null}
          onClose={() => { markAsked(naming); setNaming(null); }} />
      )}
    </div>
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
