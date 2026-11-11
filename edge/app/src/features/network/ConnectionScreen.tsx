// Bağlantı sekmesi (sahip isteği 2026-11-11): neye neyle bağlıyım, yakındaki
// cihazlar (Bluetooth / USB), kayıtların sunucuya gidişi tek cümle ve dört
// halka, bekleyen işlemler, sonra bütün cihazlar (Gateway'ler, Tasmalar).
// Bağlanınca cihazın kendi ekranı açılır; bağlantı ekrandan çıkınca kopmaz.
// Sayaçların ayrıntısı ve kayıt defteri "Teknik ayrıntılar"da.
import { useCallback, useEffect, useRef, useState, useSyncExternalStore, type ReactNode } from "react";
import {
  Bluetooth, CaretRight, CellSignalFull, CloudCheck, Cow, DeviceMobile, MagnifyingGlass, Polygon, Question,
  SlidersHorizontal, Usb, WifiHigh, WifiSlash,
} from "@phosphor-icons/react";
import { noRoleMessage, probeUsbDevice, setRoleAndReconnect, type DeviceRole, type GnssHint } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import type { FarmGateway, EdgeQueue } from "../../app/ownerAccountClient.ts";
import { collarStorageReadings } from "../../app/collarStorageReadings.ts";
import {
  deviceNames, onPilotAccountChange, onPilotObservationSync, pilotAccountStatus, pilotEdgeQueues, pilotGateways,
  pilotObservationSyncState,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { trackerSession } from "../../app/trackerSession.ts";
import { Card, Cell, Header, KV, Section, Seg } from "../../ds/components.tsx";
import { classifyNearby, mergeNearby, scanNearby, stopNearbyScan, type NearbyDevice, type NearbyView } from "../../platform/bleScan.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { AddTrackerDialog } from "../herd/AddTrackerDialog.tsx";
import { TrackerLiveDialog, type LiveTarget } from "../herd/TrackerLive.tsx";
import { NameDialog } from "./NameDialog.tsx";
import { RoleChoice } from "../devices/RoleChoice.tsx";
import { pendingKeys } from "../../app/deviceSettings.ts";
import { useDeviceSettings } from "../devices/DeviceScreen.tsx";
import { OpDialog, useOps } from "../devices/Ops.tsx";
import { groupLine, KIND_WORD, OPEN, operationSections, opWhat } from "../../app/deviceOps.ts";
import { useTrackerFences } from "../zones/ApplyFence.tsx";
import { farmChain, gatewayFacts, gatewayLine, stationWords, transferHeadline, type ChainNode } from "./words.ts";
import { transferStages } from "./transferFlow.ts";

type Via = "ble" | "usb";

const ASKED = "orun.namePrompted.v1";
function asked(): string[] {
  try { const v = JSON.parse(localStorage.getItem(ASKED) ?? "[]"); return Array.isArray(v) ? v : []; } catch { return []; }
}
function markAsked(id: string): void {
  try { localStorage.setItem(ASKED, JSON.stringify([...asked(), id.toUpperCase()].slice(-200))); } catch { /* fine */ }
}

const NODE_ICON: Record<ChainNode["key"], ReactNode> = {
  tasmalar: <Cow size={22} />, gateway: <WifiHigh size={22} />, sunucu: <CloudCheck size={22} />, telefon: <DeviceMobile size={22} />,
};

export function ConnectionScreen() {
  const m = useApp();
  const nav = useNav();
  const st = m.station, s = m.snap;
  const words = stationWords(st, s);
  const linkOn = st.link !== "none";
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const accountScope = account ? [account.site, account.actor, account.installation, account.username].join(":") : null;
  const [via, setVia] = useState<Via>("ble");
  const [gateways, setGateways] = useState<FarmGateway[] | null>(null);
  const [edgeQueues, setEdgeQueues] = useState<EdgeQueue[] | null>(null);
  const [serverConfirmed, setServerConfirmed] = useState<number | null>(null);
  const [serverReadAt, setServerReadAt] = useState<number | null>(null);
  const [edgeError, setEdgeError] = useState<string | null>(null);
  const [phoneGatewayReading, setPhoneGatewayReading] = useState<{
    id: string; held: number; capacity: number; at: number;
  } | null>(null);
  const [nearby, setNearby] = useState<NearbyDevice[]>([]);
  const [scanning, setScanning] = useState(false);
  const [note, setNote] = useState("");
  const [usbBusy, setUsbBusy] = useState(false);
  const [noRole, setNoRole] = useState<{ gnss: GnssHint } | null>(null);   // Y2a-2: kurulmamış cihaz
  const [live, setLive] = useState<{ device: string | null; target: LiveTarget; bleId?: string } | null>(null);
  const [adding, setAdding] = useState(false);
  const [naming, setNaming] = useState<string | null>(null);
  const [node, setNode] = useState<ChainNode["key"] | null>(null);
  const [opOpen, setOpOpen] = useState<string | null>(null);
  const settings = useDeviceSettings();
  const fencePlans = useTrackerFences();
  const opRows = useOps();
  const queued = new Set(opRows.filter((r) => r.kind === "fence" && OPEN.includes(r.state)).map((r) => r.device));
  const operations = operationSections(opRows);
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const collarReadings = useSyncExternalStore(collarStorageReadings.subscribe, collarStorageReadings.snapshot);
  const session = useSyncExternalStore(trackerSession.subscribe, trackerSession.snapshot);
  const [, bump] = useState(0);
  const sync = pilotObservationSyncState();
  const label = (id: string) => names[id.toUpperCase()] ?? m.herd.find((a) => a.device?.toUpperCase() === id.toUpperCase())?.name ?? bleName(id);
  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  useEffect(() => onPilotObservationSync(() => bump((n) => n + 1)), []);

  // Çiftliğin gateway'leri ve PC Edge kuyrukları: açılınca ve dakikada bir.
  useEffect(() => {
    let alive = true;
    const load = () => {
      pilotGateways().then((g) => { if (alive) setGateways(g); })
        .catch(() => { /* keep the last valid reading, with its timestamp */ });
      pilotEdgeQueues().then((q) => {
        if (alive) {
          setEdgeQueues(q?.edges ?? null);
          setServerConfirmed(q?.serverConfirmed ?? null);
          setServerReadAt(q ? Date.now() : null);
          setEdgeError(null);
        }
      }).catch((e: unknown) => {
        if (alive) setEdgeError(e instanceof Error ? e.message : "PC Edge bilgisi okunamadı.");
      });
    };
    load();
    const t = setInterval(load, 60_000);
    return () => { alive = false; clearInterval(t); };
  }, [accountScope]);
  useEffect(() => {
    // A direct BLE/USB observation is local to this phone/account session.
    // Do not leak a prior account's collar memory snapshot after sign-out.
    collarStorageReadings.setScope(accountScope);
    setPhoneGatewayReading(null);
    setGateways(null);
    setEdgeQueues(null);
    setServerConfirmed(null);
    setServerReadAt(null);
    setEdgeError(null);
  }, [accountScope]);

  // Time a Gateway HELLO/count observation when its reported value changes,
  // not every render (which would make an old reading appear permanently fresh).
  useEffect(() => {
    if (st.phase === "connected" && st.id && st.held !== null && st.capacity !== null)
      setPhoneGatewayReading({ id: st.id.toUpperCase(), held: st.held, capacity: st.capacity, at: Date.now() });
  }, [st.id, st.phase, st.held, st.capacity, accountScope]);

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

  // Bağlanınca cihazın ekranı açılır: tasma → ekranı (Bluetooth'la bağlanarak),
  // gateway → köprü olarak bağlanır ve ekranı; tanınmayan cihaz → canlı okuma.
  const pick = (d: NearbyView) => {
    void stopNearbyScan();
    if (d.kind === "gateway") {
      void m.source.connectBle(false, d.deviceId);
      if (d.id) nav.push({ to: "device", id: d.id });
      return;
    }
    if (d.kind === "tracker" && d.id) { nav.push({ to: "device", id: d.id, ble: d.deviceId }); return; }
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
      if (dev.kind === "gateway") { await m.source.connectUsb(); nav.push({ to: "device", id: dev.id }); }
      else if (role === "tasma") setAdding(true);
      else setLive({ device: dev.report.device, target: { usb: true } });
    } catch (e) {
      setNote(e instanceof Error && e.message.length < 200 ? e.message : "USB ile bağlanılamadı.");
    } finally {
      if (con) await con.close();
      setUsbBusy(false);
    }
  }

  // ---- aktarım: dört halka ve tek cümle --------------------------------
  const fresh = m.devices.filter((d) => d.lastTime && m.now - d.lastTime <= 3600_000).length;
  const chain = farmChain({
    trackers: trackers.length, fresh, gateways,
    phoneGateway: linkOn && st.phase === "connected" && st.id ? { id: st.id, link: st.link === "usb" ? "usb" : "ble" } : null,
    signedIn: Boolean(account), sync, pending: m.server.pending, now: m.now, label,
  });
  const gwHeld = (() => {
    const by = new Map<string, number>();
    for (const g of gateways ?? []) if (g.held !== null) by.set(g.gateway.toUpperCase(), g.held);
    if (linkOn && st.id && st.held !== null) by.set(st.id.toUpperCase(), st.held);
    return by.size ? [...by.values()].reduce((x, y) => x + y, 0) : null;
  })();
  const edgePending = edgeQueues ? edgeQueues.reduce((x, e) => x + e.pending, 0) : null;
  const known3 = [gwHeld, edgePending, m.server.pending].filter((x): x is number => x !== null);
  const waiting = known3.length ? known3.reduce((x, y) => x + y, 0) : null;
  const lastUpload = (gateways ?? []).reduce<number | null>((x, g) => (g.lastUpload !== null && (x === null || g.lastUpload > x) ? g.lastUpload : x), null);
  const head = transferHeadline({ signedIn: Boolean(account), sync, waiting, lastUpload, now: m.now });
  const nodes = chain.nodes.map((n) => n.key === "gateway" && gwHeld ? { ...n, sub: `${fmt.num(gwHeld)} kayıt bekliyor`, tone: "aging" as const }
    : n.key === "sunucu" && edgePending ? { ...n, detail: `${n.detail} PC Edge'de ${fmt.num(edgePending)} kayıt sunucuya gitmeyi bekliyor.` } : n);
  const picked = nodes.find((n) => n.key === node) ?? null;

  // ---- bekleyen işlemler: tasma başına tek satır --------------------------
  const pendingOps = operations.active.flatMap((g) => g.targets)
    .filter((t) => OPEN.includes(t.state) || t.state === "rejected")
    .sort((x, y) => Number(y.state === "rejected") - Number(x.state === "rejected") || y.createdAt - x.createdAt);

  // ---- cihazlar: önce sorunlular -------------------------------------------
  const trackerRows = m.herd.filter((a) => a.device).map((a) => {
    const id = a.device!, dev = m.devices.find((d) => d.id === id);
    const waitingN = pendingKeys(settings[id.toUpperCase()]).length;
    const fence = fencePlans.find((f) => f.device.toUpperCase() === id.toUpperCase());
    const staleFence = fence && (fence.state === "stale" || fence.state === "never");
    const silent = !dev?.lastTime || m.now - dev.lastTime > 3600_000;
    const connected = session?.device === id.toUpperCase() && session.link.alive;
    const bits = [connected ? "Bluetooth ile bağlı" : "",
                  dev?.lastTime ? fmt.ago(dev.lastTime, m.now) : "hiç haber yok",
                  dev && dev.batteryPercent !== null ? `%${dev.batteryPercent}` : "",
                  waitingN ? `${waitingN} ayar sırada` : "", queued.has(id.toUpperCase()) ? "alanlar sırada" : staleFence ? "alanlar değişti" : ""].filter(Boolean);
    const low = dev?.batteryPercent !== null && dev?.batteryPercent !== undefined && dev.batteryPercent < 20;
    return { id, name: a.name, sub: bits.join(" · "), connected, problem: waitingN > 0 || !!staleFence || silent || low };
  }).sort((x, y) => Number(y.connected) - Number(x.connected) || Number(y.problem) - Number(x.problem) || x.name.localeCompare(y.name, "tr"));

  const gwRows = (() => {
    const list = [...(gateways ?? [])];
    if (linkOn && st.id && !list.some((g) => g.gateway.toUpperCase() === st.id!.toUpperCase()))
      list.unshift({ gateway: st.id.toUpperCase(), via: st.link === "usb" ? "phone-usb" : "phone-ble", held: st.held, capacity: st.capacity,
                     lastSeen: m.now, statusAt: null, lastUpload: null, trackers24h: 0 });
    return list;
  })();
  const isBridged = (g: FarmGateway) => linkOn && !!st.id && g.gateway.toUpperCase() === st.id.toUpperCase();

  const flow = transferStages({
    now: Date.now(),
    trackerIds: m.herd.filter((a) => a.device).map((a) => a.device!),
    collars: Object.values(collarReadings),
    gateways,
    localGateway: {
      id: st.id, connected: linkOn && st.phase === "connected",
      held: st.held, capacity: st.capacity,
      readAt: phoneGatewayReading && phoneGatewayReading.id === st.id?.toUpperCase()
        ? phoneGatewayReading.at : null,
    },
    edges: edgeQueues,
    serverConfirmed,
    serverReadAt,
    label,
  });

  const sessionOn = !!session && session.link.alive;
  return (
    <div className="page">
      <Header title={tr.network.title} />

      {(linkOn || sessionOn) && <>
        <Section title="Bağlı" info="Bu telefonun şu an bağlı olduğu cihazlar. Bağlantı ekrandan çıkınca kopmaz; dokununca cihazın ekranı ve ayarları açılır, oradan kesilir." />
        <Card>
          {linkOn && (
            <Cell lead={<span className={"ic " + (words.dot === "ok" ? "fresh" : "aging")}>{st.link === "usb" ? <Usb size={22} /> : <Bluetooth size={22} />}</span>}
              title={st.id ? label(st.id) : "Gateway"} wrapSub
              sub={st.phase === "connected"
                ? `Gateway · ${st.link === "usb" ? "USB" : "Bluetooth"} ile bağlı${st.held !== null ? ` · ${fmt.num(st.held)} kayıt bellekte` : ""}`
                : `Gateway · ${words.text}${st.detail ? ` · ${st.detail}` : ""}`}
              subTone={st.phase === "connected" ? "fresh" : "aging"}
              trail={<span className="sm row" style={{ gap: 2, color: "var(--brand-t)" }}>Ayarlar<CaretRight size={16} /></span>}
              onClick={() => (st.id ? nav.push({ to: "device", id: st.id }) : undefined)} />
          )}
          {sessionOn && (
            <Cell lead={<span className="ic fresh"><Bluetooth size={22} /></span>}
              title={session!.device ? label(session!.device) : bleName(session!.bleId)} wrapSub
              sub={`Tasma · Bluetooth ile bağlı · ${fmt.since(session!.since, Math.max(m.now, session!.since))}`} subTone="fresh"
              trail={<span className="sm row" style={{ gap: 2, color: "var(--brand-t)" }}>Ayarlar<CaretRight size={16} /></span>}
              onClick={() => (session!.device ? nav.push({ to: "device", id: session!.device })
                : setLive({ device: null, target: { ble: session!.bleId }, bleId: session!.bleId }))} />
          )}
        </Card>
      </>}

      <div style={{ padding: "14px 16px 0" }}>
        <Seg<Via> value={via} onChange={setVia}
          options={[{ value: "ble", label: tr.station.ble }, { value: "usb", label: tr.station.usb }]} />
      </div>
      {via === "ble" ? (
        <>
          <Section title="Yakındaki cihazlar" action={scanning ? "Taranıyor…" : "Tara"} onAction={() => { void scan(); }} />
          <Card>
            {views.length ? views.map((d) => (
              <Cell key={d.deviceId}
                lead={<span className={"ic " + (d.kind === "gateway" ? "fresh" : d.kind === "tracker" ? "brand" : "")}>
                  {d.kind === "gateway" ? <WifiHigh size={20} /> : d.kind === "tracker" ? <CellSignalFull size={20} /> : <Question size={20} />}
                </span>}
                title={d.label}
                sub={[d.kind === "gateway" ? "Gateway" : d.kind === "tracker" ? "Tasma" : "Tanınmayan ORUN cihazı",
                      d.rssi !== null ? `${d.rssi} dBm` : ""].filter(Boolean).join(" · ")}
                trail={<span className="sm row" style={{ gap: 2, color: "var(--brand-t)" }}>Bağlan<CaretRight size={16} /></span>}
                onClick={() => pick(d)} />
            )) : (
              <div className="stack" style={{ padding: "18px 16px", alignItems: "center", textAlign: "center" }}>
                <Bluetooth size={24} />
                <span className="sm">{scanning ? "Yakındaki ORUN cihazları aranıyor…" : "Henüz cihaz görülmedi"}</span>
                {!scanning && <button className="btn small" onClick={() => { void scan(); }}><MagnifyingGlass size={18} />Tara</button>}
              </div>
            )}
          </Card>
          {!linkOn && <div style={{ padding: "6px 16px 0" }}>
            <button className="btn quiet small" disabled={!s.ready} onClick={() => m.source.connectBle(true)}>Android listesinden gateway seç</button>
          </div>}
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

      <Section title="Aktarım" info="Kayıt tasmadan gateway'e, oradan köprüyle (PC ya da telefon) sunucuya gider; sunucudan bu telefona eşitlenir. Bir halka kaydı ancak sonraki halka kalıcı yazınca siler. Halkaya dokun, ayrıntısı açılır." />
      <Card>
        <div className="xfer-head">
          <span className={"dot " + head.tone} />
          <span className="strong">{head.text}</span>
        </div>
        <div className="chain">
          {nodes.map((n, i) => (
            <span key={n.key} style={{ display: "contents" }}>
              {i > 0 && <span className={"wire " + (chain.wires[i - 1] ?? "")} />}
              <button className="node" aria-pressed={node === n.key} onClick={() => setNode(node === n.key ? null : n.key)}>
                <span className={"nd ic " + n.tone}>{NODE_ICON[n.key]}</span>
                <b>{n.title}</b>
                <span>{n.sub}</span>
              </button>
            </span>
          ))}
        </div>
        {picked ? <p className="sm chain-detail">{picked.detail}</p> : <div style={{ height: 8 }} />}
      </Card>
      {edgeError && account && <p className="fine lead" role="status">PC Edge durumu okunamadı: {edgeError}</p>}

      <Section title={pendingOps.length ? `Bekleyen işlemler · ${pendingOps.length}` : "Bekleyen işlemler"}
        info="Hesaba kaydedilen ayar ve alanlar. Tasmaya bağlanınca (USB ya da Bluetooth) sormadan yazılır; 'Tasmaya işlendi' yalnız tasma onaylayınca olur. Satıra dokun, tasmanın ekranı açılır." />
      {pendingOps.length ? (
        <Card>
          {pendingOps.slice(0, 8).map((t) => (
            <Cell key={t.op + t.device}
              lead={<span className={"ic " + (t.state === "rejected" ? "alarm" : "aging")}>{t.kind === "fence" ? <Polygon size={20} /> : <SlidersHorizontal size={20} />}</span>}
              title={label(t.device)} wrapSub
              sub={`${opWhat(t)} · ${t.state === "rejected" ? `reddedildi${t.note ? ` (${t.note})` : ""}` : t.state === "gateway" ? "gateway'de, tasmaya gidiyor" : "tasmaya bağlanınca gider"}`}
              subTone={t.state === "rejected" ? "alarm" : undefined}
              trail={<CaretRight size={18} color="var(--mute)" />}
              onClick={() => nav.push({ to: "device", id: t.device })} />
          ))}
          {pendingOps.length > 8 && <p className="fine" style={{ padding: "10px 16px", margin: 0 }}>{pendingOps.length - 8} işlem daha; tasmaların ekranında görünür.</p>}
        </Card>
      ) : <p className="sm lead">Bekleyen işlem yok. Bütün ayar ve alanlar tasmalarda.</p>}

      <Section title={`Gateway'ler${gwRows.length ? ` · ${gwRows.length}` : ""}`} />
      <Card>
        {!account && !gwRows.length ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Hesaba giriş yapınca çiftliğin gateway'leri burada görünür.</p>
          : account && gateways === null && !gwRows.length ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Okunuyor…</p>
          : !gwRows.length ? <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Henüz gateway yok.</p>
          : gwRows.map((g) => (
            <Cell key={g.gateway}
              lead={<span className={"ic " + (g.via || isBridged(g) ? "fresh" : "")}>{g.via || isBridged(g) ? <WifiHigh size={20} /> : <WifiSlash size={20} />}</span>}
              title={label(g.gateway)} wrapSub
              sub={isBridged(g) ? `Bu telefona ${st.link === "usb" ? "USB" : "Bluetooth"} ile bağlı${g.held !== null ? ` · ${fmt.num(g.held)} kayıt bellekte` : ""}`
                : <>{cap(gatewayLine(g, m.now))}<br />{gatewayFacts(g)}</>}
              subTone={isBridged(g) ? "fresh" : undefined}
              trail={<CaretRight size={18} color="var(--mute)" />}
              onClick={() => nav.push({ to: "device", id: g.gateway })} />
          ))}
      </Card>

      <Section title={`Tasmalar${trackerRows.length ? ` · ${trackerRows.length}` : ""}`}
        info="Önce bağlı olan, sonra bakılması gerekenler (sessiz, pili düşük, bekleyen ayar ya da alan). Dokun: durum, bağlan, ayarlar." />
      <Card>
        {trackerRows.length ? trackerRows.map((r) => (
          <Cell key={r.id}
            lead={<span className={"ic " + (r.connected ? "fresh" : r.problem ? "aging" : "brand")}>{r.connected ? <Bluetooth size={20} /> : <CellSignalFull size={20} />}</span>}
            title={r.name} wrapSub sub={r.sub} subTone={r.connected ? "fresh" : r.problem ? "aging" : undefined}
            trail={<CaretRight size={18} color="var(--mute)" />}
            onClick={() => nav.push({ to: "device", id: r.id })} />
        )) : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Henüz tasma yok. Sürü → "+" ile ekle.</p>}
      </Card>

      <TechDetails flow={flow} history={operations.history} onOp={setOpOpen} />
      {opOpen && <OpDialog op={opOpen} onClose={() => setOpOpen(null)} />}

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

function TechDetails({ flow, history, onOp }: {
  flow: ReturnType<typeof transferStages>; history: ReturnType<typeof operationSections>["history"]; onOp: (op: string) => void;
}) {
  const m = useApp();
  const s = m.snap, c = s.counts, sv = m.server;
  const end = useRef<HTMLDivElement>(null);
  return (
    <details className="tech" onToggle={() => end.current?.scrollIntoView({ block: "end" })}>
      <summary>{tr.common.tech}</summary>
      <Section title="Sayaçlar" info="Tasma/Gateway: kayıt sayısı / bellek kapasitesi. PC Edge: sunucuya aktarılmayı bekleyen kayıtlar. Sunucu: doğrulanmış benzersiz kayıtlar. Eski ölçümler canlı değer değildir; sayılar toplanmaz." />
      <Card>
        {flow.flatMap((stage) => stage.entries.map((e) => (
          <Cell key={stage.id + e.id} title={`${stage.title.replace(/^\d · /, "")} · ${e.title}`} wrapSub
            sub={e.detail} subTone={e.stale ? "aging" : undefined}
            trail={<span className="sm strong" style={{ flex: "none", fontVariantNumeric: "tabular-nums" }}>{e.amount}</span>} />
        )))}
      </Card>
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
      {history.length > 0 && <>
        <Section title={`İşlem geçmişi · ${history.length}`} />
        <Card>{history.slice(0, 20).map((g) => (
          <Cell key={g.op} title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`} sub={groupLine(g)} onClick={() => onOp(g.op)} />
        ))}</Card>
      </>}
      <Section title={tr.tech.log} />
      <div className="log" role="log">{s.log.join("\n")}<div ref={end} /></div>
    </details>
  );
}

