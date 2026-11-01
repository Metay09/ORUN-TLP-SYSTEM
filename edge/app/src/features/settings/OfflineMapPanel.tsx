// İnternetsiz harita: sunucudaki Aydın–Muğla haritasını telefona indirir.
// İndirilen harita internet yokken kendiliğinden gösterilir.
import { useEffect, useRef, useState } from "react";
import { onPilotAccountChange, pilotAccountStatus, pilotMapClient } from "../../app/ownerAccountPilotRuntime.ts";
import { Bar, Card, KV, Section } from "../../ds/components.tsx";
import {
  packDate, packOffers, packSize, type InstalledPack, type OfflinePack, type PackOffer,
} from "../map/offlinePacks/catalog.ts";
import { offlineMapsChanged } from "../map/offlinePacks/offlineMap.ts";
import {
  installOfflinePack, listInstalledPacks, removeOfflinePack, supportsOfflineMapStorage,
} from "../map/offlinePacks/store.ts";

function message(error: unknown): string {
  if (error instanceof DOMException && error.name === "AbortError") return "İndirme durduruldu.";
  if (error instanceof Error && error.message.length <= 180) return error.message;
  return "Harita işlemi tamamlanamadı.";
}

export function OfflineMapPanel() {
  const supported = supportsOfflineMapStorage();
  const [account, setAccount] = useState(() => pilotAccountStatus());
  const [installed, setInstalled] = useState<InstalledPack[]>([]);
  const [catalog, setCatalog] = useState<OfflinePack[] | null>(null);
  const [progress, setProgress] = useState<{ title: string; ratio: number; got: number; total: number } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const abort = useRef<AbortController | null>(null);

  useEffect(() => onPilotAccountChange(() => setAccount(pilotAccountStatus())), []);
  useEffect(() => () => abort.current?.abort(), []);

  async function refresh(): Promise<void> {
    if (!supported) return;
    setInstalled(await listInstalledPacks());
    const client = pilotMapClient();
    setCatalog(client ? await client.mapCatalog() : null);
  }

  useEffect(() => {
    refresh().catch((e: unknown) => setError(message(e)));
  }, [account?.username]); // eslint-disable-line react-hooks/exhaustive-deps

  async function install(offer: PackOffer): Promise<void> {
    const client = pilotMapClient();
    if (busy || !client) return;
    setBusy(true); setError(""); setNotice("");
    const ctl = new AbortController();
    abort.current = ctl;
    let shown = 0;
    try {
      const done = await installOfflinePack(offer.pack, ({ receivedBytes, totalBytes }) => {
        const now = Date.now();
        if (now - shown < 250 && receivedBytes < totalBytes) return;
        shown = now;
        setProgress({ title: offer.pack.title, ratio: receivedBytes / totalBytes, got: receivedBytes, total: totalBytes });
      }, ctl.signal, client.mapDownloadHeaders());
      // Older versions go only after the new one is verified on the phone.
      for (const old of installed)
        if (old.pack.id === done.pack.id && old.fileName !== done.fileName)
          await removeOfflinePack(old).catch(() => undefined);
      offlineMapsChanged();
      setNotice("Harita telefonda. İnternet yokken kendiliğinden gösterilir.");
    } catch (e: unknown) {
      setError(message(e));
    } finally {
      abort.current = null;
      setProgress(null);
      setBusy(false);
      await refresh().catch(() => undefined);
    }
  }

  async function remove(p: InstalledPack): Promise<void> {
    if (busy) return;
    setBusy(true); setError(""); setNotice("");
    try {
      await removeOfflinePack(p);
      offlineMapsChanged();
      setNotice("Harita telefondan silindi.");
    } catch (e: unknown) {
      setError(message(e));
    } finally {
      setBusy(false);
      await refresh().catch(() => undefined);
    }
  }

  const offers = catalog ? packOffers(installed, catalog) : [];
  return (
    <>
      <Section title="İnternetsiz harita" />
      <Card>
        <div className="stack" style={{ padding: 16 }}>
          {!supported && <p className="sm">Bu telefonun tarayıcı bileşeni harita dosyasını saklayamıyor. Android System WebView'i güncelle.</p>}
          {supported && installed.map((p) => (
            <div key={p.fileName} className="stack">
              <KV k={p.pack.title}>{packSize(p.pack.bytes)} · {packDate(p.pack.version)}</KV>
              <button className="btn quiet" type="button" disabled={busy} onClick={() => void remove(p)}>Telefondan sil</button>
            </div>
          ))}
          {supported && offers.map((o) => (
            <div key={o.pack.id + o.pack.version} className="stack">
              {!o.installed && <KV k={o.pack.title}>{packSize(o.pack.bytes)} · {packDate(o.pack.version)}</KV>}
              <button className="btn primary" type="button" disabled={busy} onClick={() => void install(o)}>
                {o.installed ? `Yeni sürümü indir (${packDate(o.pack.version)})` : `İndir (${packSize(o.pack.bytes)})`}
              </button>
            </div>
          ))}
          {progress && (
            <div className="stack">
              <span className="sm">{progress.title}: %{Math.floor(progress.ratio * 100)} · {packSize(progress.got)} / {packSize(progress.total)}</span>
              <Bar ratio={progress.ratio} />
              <button className="btn quiet small" type="button" onClick={() => abort.current?.abort()}>Durdur</button>
              <p className="fine">Uygulama açıkken iner; Wi-Fi önerilir.</p>
            </div>
          )}
          {supported && !account && installed.length === 0 &&
            <p className="sm">Haritayı indirmek için önce hesaba giriş yap.</p>}
          {supported && account && catalog && catalog.length === 0 && installed.length === 0 &&
            <p className="sm">Sunucuda henüz harita paketi yok.</p>}
          {supported && installed.length > 0 && offers.length === 0 && !progress &&
            <p className="fine">İnternet yokken uydu ve arazi yerine kendiliğinden bu harita gösterilir. Sokak haritası her zaman bunu kullanır.</p>}
          {notice && <p role="status" className="sm">{notice}</p>}
          {error && <p role="alert" className="sm t-aging">{error}</p>}
        </div>
      </Card>
    </>
  );
}
