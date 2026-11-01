// Plain sentences for the station link and the server, shared by the map
// status chip, the network tab and the settings page.
import type { Snapshot } from "../../app/controller.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import type { ServerView, Station } from "../../app/model.ts";

/** `lastArrival`: newest record reaching the phone (shown as "· 14:52"). */
export function stationWords(st: Station, s: Snapshot, lastArrival: number | null = null): { dot: "ok" | "wait" | "grey"; text: string; short: string } {
  if (!s.ready) return { dot: "grey", text: s.error ? tr.states.ledgerError(s.error) : tr.states.opening, short: tr.states.opening };
  const via = st.link === "usb" ? tr.station.usb : tr.station.ble;
  if (st.link !== "none" && st.phase === "connected") {
    const text = tr.station.connected(via);
    return { dot: "ok", text, short: lastArrival ? `${tr.station.connectedShort} · ${fmt.clock(lastArrival)}` : text };
  }
  if (st.link !== "none") {
    const t = st.phase === "waiting" ? tr.station.waiting : tr.station.searching;
    return { dot: "wait", text: t, short: t };
  }
  return { dot: "grey", text: tr.station.notConnected, short: tr.station.notConnected };
}

/** Status for THIS phone only; a remote PC Edge may deliver independently. */
export function phonePathWords(
  st: Pick<Station, "link" | "phase">,
  snap: Pick<Snapshot, "ready" | "records">,
): { stationConnected: boolean; stationText: string; phoneText: string; phoneTone: "fresh" | "aging" | "" } {
  const stationConnected = snap.ready && st.link !== "none" && st.phase === "connected";
  return {
    stationConnected,
    stationText: stationConnected ? "Bu telefon gateway'e bağlı" : "Bu telefon bağlı değil",
    phoneText: !snap.ready ? "Yerel defter hazır değil"
      : snap.records === null ? "Yerel defter açık · sayı bekleniyor"
      : snap.records === 0 ? "Henüz yerel kayıt yok"
      : `${fmt.num(snap.records)} yerel kayıt`,
    phoneTone: !snap.ready ? "" : (snap.records ?? 0) > 0 ? "fresh" : "aging",
  };
}

/** The account login and a successful ping do not certify an upload.
 * These counters belong to the phone ledger, NOT the PC Edge or whole site.
 */
export function serverWords(sv: ServerView): { tone: "ok" | "up" | "off" | "none"; text: string } {
  if (!sv.configured) return { tone: "none", text: tr.server.notSet };
  if (sv.busy) return { tone: "up", text: tr.server.sending };
  if (sv.error) return { tone: "off", text: "Bu telefonun sunucu işlemi başarısız" };
  if (sv.conflict > 0 || sv.rejected > 0)
    return { tone: "off", text: "Bu telefonda çakışan veya reddedilen kayıt var" };
  if ((sv.pending ?? 0) > 0)
    return { tone: "up", text: tr.server.pending(sv.pending ?? 0, false) };
  if (sv.pending === 0 && sv.lastOk && (sv.uploaded ?? 0) > 0)
    return { tone: "ok", text: "Bu telefonun kayıtları sunucuda onaylı" };
  if (sv.pending === 0 && sv.lastOk)
    return { tone: "up", text: "Sunucu erişimi doğrulandı · kayıt teslimi yok" };
  return { tone: "up", text: "Bu telefonun sunucu teslimi henüz doğrulanmadı" };
}

/** Bağlantı sekmesinin rengi: veri bu telefona geliyor mu?
 * ok: gateway bağlı ya da sunucuyla son 5 dakikada eşitlendi.
 * bad: ne gateway var ne hesap, ya da son eşitleme hata verdi (internet yok).
 */
export function connectionTone(i: {
  gatewayConnected: boolean; signedIn: boolean; syncError: boolean;
  lastSyncAt: number | null; now: number;
}): "ok" | "wait" | "bad" {
  if (i.gatewayConnected) return "ok";
  if (!i.signedIn || i.syncError) return "bad";
  if (i.lastSyncAt !== null && i.now - i.lastSyncAt < 5 * 60_000) return "ok";
  return "wait";
}
