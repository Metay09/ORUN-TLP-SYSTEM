// Bağlantı sekmesi (sahip kararı 2026-11-10; 2 Kasım'da Cihazlar'dı): üstte "Kayıt
// güvende mi?" ve "Bu telefon" kartı; Bluetooth / USB seçici ve yakındaki ORUN
// cihazları; sonra bütün cihazlar türüne göre (Gateway'ler, Tasmalar; ileride
// Sensörler, Vanalar), her biri kendi ekranına açılır; gruplar; teknik ayrıntılar.
import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";
import {
  Bluetooth, CaretRight, CellSignalFull, MagnifyingGlass, MapPin, Question, Usb, WifiHigh, WifiSlash, X,
} from "@phosphor-icons/react";
import { noRoleMessage, probeUsbDevice, setRoleAndReconnect, type DeviceRole, type GnssHint } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import type { FarmGateway, EdgeQueue } from "../../app/ownerAccountClient.ts";
import { collarStorageReadings } from "../../app/collarStorageReadings.ts";
import {
  deviceNames, onPilotAccountChange, pilotAccountStatus, pilotGateways, pilotEdgeQueues,
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
import { groupLine, operationSections, KIND_WORD } from "../../app/deviceOps.ts";
import { useTrackerFences } from "../zones/ApplyFence.tsx";
import { gatewayFacts, gatewayLine, stationWords } from "./words.ts";
import { transferStages } from "./transferFlow.ts";

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
  const st = m.station, s = m.snap;
  const words = stationWords(st, s);
  const linkOn = st.link !== "none";
  const held = st.held ?? 0;
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const accountScope = account ? [account.site, account.actor, account.installation, account.username].join(":") : null;
  const [via, setVia] = useState<Via>("ble");
  const [marking, setMarking] = useState(false);
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
  const [bulk, setBulk] = useState(false);
  const settings = useDeviceSettings();
  const fencePlans = useTrackerFences();
  const opRows = useOps();
  const queued = new Set(opRows.filter((r) => r.kind === "fence" && (r.state === "waiting" || r.state === "gateway")).map((r) => r.device));
  const operations = operationSections(opRows);
  const groups = operations.active;
  const [opOpen, setOpOpen] = useState<string | null>(null);
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const collarReadings = useSyncExternalStore(collarStorageReadings.subscribe, collarStorageReadings.snapshot);
  const label = (id: string) => names[id.toUpperCase()] ?? bleName(id);
  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  // Çiftliğin gateway'leri: açılınca ve dakikada bir.
  useEffect(() => {
    let live = true;
    const load = () => {
      pilotGateways().then((g) => { if (live) setGateways(g); })
        .catch(() => { /* keep the last valid reading, with its timestamp */ });
      pilotEdgeQueues().then((q) => {
        if (live) {
          setEdgeQueues(q?.edges ?? null);
          setServerConfirmed(q?.serverConfirmed ?? null);
          setServerReadAt(q ? Date.now() : null);
          setEdgeError(null);
        }
      }).catch((e: unknown) => {
        if (live) setEdgeError(e instanceof Error ? e.message : "PC Edge bilgisi okunamadı.");
      });
    };
    load();
    const t = setInterval(load, 60_000);
    return () => { live = false; clearInterval(t); };
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
  const flow = transferStages({
    now: Date.now(),
    trackerIds: m.herd.filter((a) => a.device).map((a) => a.device!),
    collars: Object.values(collarReadings),
    gateways,
    localGateway: {
      id: st.id, connected: linkOn && st.phase === "connected",
      held: st.held, capacity: st.capacity,
      readAt: phoneGatewayReading?.id === st.id?.toUpperCase() ? phoneGatewayReading.at : null,
    },
    edges: edgeQueues,
    serverConfirmed,
    serverReadAt,
    label,
  });
  return (
    <div className="page">
      <Header title={tr.network.title} />

      <Section title="Kayıt aktarımı"
        info="Tasma → Gateway → PC Edge → Sunucu. Her aşamanın sayısı farklı anlama gelir; toplanmaz. Son bilinen ölçüm, eskidiğinde de zamanıyla gösterilir." />
      <Card>
        {flow.map((stage, i) => (
          <div key={stage.id} style={{ padding: "14px 16px", borderBottom: i < flow.length - 1 ? "1px solid var(--line)" : undefined }}>
            <div className="row" style={{ justifyContent: "space-between", marginBottom: 10 }}>
              <span className="strong">{stage.title}</span>
              <span className="fine">{i < 3 ? "↓" : "✓"}</span>
            </div>
            <div className="stack" style={{ gap: 10 }}>
              {stage.entries.map((entry) => (
                <div key={entry.id} className="row" style={{ alignItems: "flex-start", justifyContent: "space-between", gap: 12 }}>
                  <div className="stack" style={{ gap: 3, flex: 1, minWidth: 0 }}>
                    <span className="sm strong">{entry.title}</span>
                    <span className={"fine" + (entry.stale ? " t-aging" : "")}>{entry.detail}</span>
                  </div>
                  <span className="strong" style={{ flex: "none", textAlign: "right", fontVariantNumeric: "tabular-nums" }}>{entry.amount}</span>
                </div>
              ))}
            </div>
          </div>
        ))}
      </Card>
      <p className="fine lead">Tasma/Gateway: kayıt sayısı / bellek kapasitesi. PC Edge: sunucuya aktarılmayı bekleyen kayıtlar. Sunucu: doğrulanmış benzersiz kayıtlar. Eski ölçümler canlı değer değildir.</p>
      {edgeError && account && <p className="sm t-alarm lead" role="status">PC Edge durumu: {edgeError}</p>}

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
        <Section title={`Bekleyen / sorunlu işlemler (${groups.length})`}
          info="Yalnız açık işlemler ve son denemesi reddedilenler. Tasma onayı olmadan tamamlandı sayılmaz." />
        <Card>
          {groups.slice(0, 5).map((g) => (
            <Cell key={g.op} title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`} wrapSub sub={groupLine(g)}
              subTone={g.targets.some((t) => t.state === "rejected") ? "alarm" : undefined}
              trail={<CaretRight size={18} color="var(--mute)" />} onClick={() => setOpOpen(g.op)} />
          ))}
        </Card>
        {groups.length > 5 && <details className="tech">
          <summary>{groups.length - 5} açık işlem daha</summary>
          <Card>{groups.slice(5).map((g) => (
            <Cell key={g.op} title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`}
              sub={groupLine(g)} onClick={() => setOpOpen(g.op)} />
          ))}</Card>
        </details>}
      </>}
      {operations.history.length > 0 && <details className="tech">
        <summary>İşlem geçmişi ({operations.history.length})</summary>
        <Card>{operations.history.slice(0, 20).map((g) => (
          <Cell key={g.op} title={`${KIND_WORD[g.kind]} · ${fmt.dateTime(g.createdAt)}`}
            sub={groupLine(g)} onClick={() => setOpOpen(g.op)} />
        ))}</Card>
        {operations.history.length > 20 && <p className="fine lead">Son 20 işlem gösteriliyor; diğer kayıtlar silinmedi.</p>}
      </details>}
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
