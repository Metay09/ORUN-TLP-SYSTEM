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

export function serverWords(sv: ServerView): { tone: "ok" | "up" | "off" | "none"; text: string } {
  if (!sv.configured) return { tone: "none", text: tr.server.notSet };
  if (sv.busy) return { tone: "up", text: tr.server.sending };
  if ((sv.pending ?? 0) > 0) return { tone: sv.error ? "off" : "up", text: tr.server.pending(sv.pending ?? 0, Boolean(sv.error)) };
  if (sv.pending !== null && sv.lastOk)
    return { tone: "ok", text: tr.server.allSent(sv.lastOk.slice(0, 5)) };
  // Password authentication is not evidence that the observation relay
  // has actually accepted any upload. Never show a false green connection.
  return { tone: "up", text: "Sunucu aktarımı henüz doğrulanmadı" };
}
