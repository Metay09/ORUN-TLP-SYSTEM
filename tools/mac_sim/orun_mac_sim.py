#!/usr/bin/env python3
"""ORUN MAC v0 timing simulator (host-only; no firmware, no RF claims).

Discrete-event model of the ORUN MAC v0 rules in
docs/architecture/ORUN_MAC_V0_DECISIONS.md:

  R1  fixed ACK rendezvous: tracker listens only at TX_end + X (+/- w/2)
  R2  missed ACK never loses data: record stays, exact retry next wake
  R3  stop-and-wait per record, at most N_MAX records per wake,
      live record first, then oldest backlog; stop on first missed ACK
  R4  route hint via the existing authenticated path_flags RELAY_ALLOWED:
      cleared after a direct ACK, set after a relayed ACK or a miss
  R5  Gateway dedupe by (device, seq); one ACK path per attempt
  R6  Relay holds the Gateway ACK and transmits it at the tracker's slot

PHY: standard Semtech LoRa time-on-air equation.  Collision model is
deliberately pessimistic: any time overlap of two frames audible at a
receiver destroys the reception there (no capture effect).  Half-duplex:
a node never receives while it transmits.

Frame sizes come from ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md:
PERIODIC 100 B, V2_RELAY_FORWARD wrapper +16 B, GATEWAY_CUSTODY_ACK 56 B.

Usage:
  python3 orun_mac_sim.py                 # run the standard scenario table
  python3 orun_mac_sim.py --scenario S6   # one scenario
  python3 orun_mac_sim.py --json out.json # also write machine-readable results
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import random
import statistics
import sys
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# PHY
# --------------------------------------------------------------------------

PERIODIC_B = 100
WRAP_B = 16
ACK_B = 56


def lora_airtime_s(payload: int, sf: int, bw_hz: int = 125_000, cr_denom_minus4: int = 1,
                   preamble: int = 8, explicit: bool = True, crc: bool = True) -> float:
    """Semtech SX126x time on air. LDRO auto-on for SF11/12 at BW125 (as the driver does)."""
    tsym = (2 ** sf) / bw_hz
    de = 1 if (sf >= 11 and bw_hz == 125_000) else 0
    ih = 0 if explicit else 1
    num = 8 * payload - 4 * sf + 28 + (16 if crc else 0) - 20 * ih
    den = 4 * (sf - 2 * de)
    n_payload = 8 + max(math.ceil(num / den) * (cr_denom_minus4 + 4), 0)
    return (preamble + 4.25) * tsym + n_payload * tsym


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

@dataclass
class Config:
    name: str = "custom"
    sf: int = 11
    hours: float = 24.0
    interval_s: float = 15 * 60          # moving cadence (worst case all day)
    jitter_frac: float = 0.10            # per-report random offset, +/- fraction of interval
    retry_in_wake: int = 1               # extra tries after a missed ACK, same wake
    retry_backoff: tuple = (2.0, 8.0)    # random wait before that retry
    n_max: int = 4                       # records per wake
    window_s: float = 0.5                # tracker RX window width (ACK preamble must start inside)
    x_s: float | None = None             # ACK slot offset; None -> derived minimum
    relay_delay: tuple = (1.2, 4.2)      # current V1 relay_config bounds
    gw_commit: tuple = (0.1, 0.5)        # Gateway durable commit/readback latency
    relay_queue: int = 4
    retry_dedupe_s: float = 30.0         # same (dev,seq) inside this = same attempt
    history_cap: int = 728
    outage: tuple | None = None          # (start_h, end_h) Gateway radio off
    groups: dict = field(default_factory=lambda: {"direct": 1})
    seeds: int = 5
    duty_limit: float = 0.10             # BTK 869.4-869.65 MHz row (no LBT+AFA)
    cad: bool = False                    # listen-before-talk (CAD) before non-slot TX
    cad_backoff: tuple = (0.3, 2.0)
    cad_max_tries: int = 8

    def airtimes(self):
        return {
            "up": lora_airtime_s(PERIODIC_B, self.sf),
            "upw": lora_airtime_s(PERIODIC_B + WRAP_B, self.sf),
            "ack": lora_airtime_s(ACK_B, self.sf),
            "ackw": lora_airtime_s(ACK_B + WRAP_B, self.sf),
        }

    def x_min(self) -> float:
        a = self.airtimes()
        guard = 1.0
        return self.relay_delay[1] + a["upw"] + self.gw_commit[1] + a["ack"] + guard

    def x(self) -> float:
        return self.x_s if self.x_s is not None else math.ceil(self.x_min())


# group name -> (hears_gateway, hears_relay)
GROUPS = {
    "direct": (True, False),
    "both": (True, True),
    "relay": (False, True),
}


# --------------------------------------------------------------------------
# Simulation
# --------------------------------------------------------------------------

@dataclass
class Tx:
    id: int
    src: str
    start: float
    end: float
    kind: str          # up | upw | ack (direct G->T, or G->R when relay flag) | ackw
    data: dict


class Sim:
    def __init__(self, cfg: Config, seed: int):
        self.cfg = cfg
        self.rng = random.Random(seed)
        self.a = cfg.airtimes()
        self.X = cfg.x()
        self.now = 0.0
        self.q: list = []
        self.seq_counter = 0
        self.txs: list[Tx] = []
        self.tx_id = 0
        self.end_t = cfg.hours * 3600
        self.busy: dict[str, list[tuple[float, float]]] = {}
        self.tx_time_bins: dict[str, dict[int, float]] = {}
        self.stats = {k: 0 for k in [
            "records", "lost_capacity", "attempts", "acked",
            "miss_no_uplink", "miss_relay_late", "miss_slot_clash", "miss_ack_lost",
            "relay_queue_drop", "uplink_rx_fail_gw", "uplink_rx_fail_relay",
            "dup_copies_gw", "relay_forwards", "gw_acks", "cad_backoffs", "miss_outage",
        ]}
        self.cad_tries: dict = {}
        self.latencies: list[float] = []
        self.backlog_samples: list[tuple[float, int, int]] = []
        self._build()

    # ---------------- topology ----------------
    def _build(self):
        self.trackers: dict[str, dict] = {}
        idx = 0
        for gname, count in self.cfg.groups.items():
            hg, hr = GROUPS[gname]
            for _ in range(count):
                tid = f"T{idx}"
                idx += 1
                self.trackers[tid] = {
                    "group": gname, "hg": hg, "hr": hr,
                    "queue": [], "seq": 0, "in_wake": False,
                    "relay_allowed": not hg,       # first guess: direct if it can
                    "attempt": None, "plan": [], "sent_in_wake": 0,
                    "tx_s": 0.0, "rx_s": 0.0,
                }
                phase = self.rng.uniform(0, self.cfg.interval_s)
                self.push(phase, "report", tid)
        self.has_relay = any(t["hr"] for t in self.trackers.values())
        self.relay = {"pending": 0, "hold": {}, "dedupe": {}}
        self.gw = {"seen": {}, "committed": set()}
        for n in list(self.trackers) + ["G", "R"]:
            self.busy[n] = []
            self.tx_time_bins[n] = {}
        self.push(0.0, "sample", None)

    def hears(self, a: str, b: str) -> bool:
        if a == b:
            return False
        if {a, b} == {"G", "R"}:
            return self.has_relay
        if a in ("G", "R") or b in ("G", "R"):
            t, other = (b, a) if a in ("G", "R") else (a, b)
            tr = self.trackers[t]
            return tr["hg"] if other == "G" else tr["hr"]
        ta, tb = self.trackers[a], self.trackers[b]
        return (ta["hg"], ta["hr"]) == (tb["hg"], tb["hr"])

    def gw_online(self, t: float) -> bool:
        o = self.cfg.outage
        return not (o and o[0] * 3600 <= t < o[1] * 3600)

    # ---------------- event queue ----------------
    def push(self, t: float, kind: str, data):
        self.seq_counter += 1
        heapq.heappush(self.q, (t, self.seq_counter, kind, data))

    def run(self):
        while self.q:
            t, _, kind, data = heapq.heappop(self.q)
            if t > self.end_t:
                break
            self.now = t
            getattr(self, "ev_" + kind)(data)
        return self

    # ---------------- airtime bookkeeping ----------------
    def free_at(self, node: str, t: float, dur: float) -> float:
        s = t
        for (b0, b1) in sorted(self.busy[node]):
            if b1 <= s:
                continue
            if b0 >= s + dur:
                break
            s = b1 + 0.02
        return s

    def overlaps_busy(self, node: str, t0: float, t1: float) -> bool:
        return any(b0 < t1 and b1 > t0 for (b0, b1) in self.busy[node])

    def channel_busy(self, node: str) -> bool:
        """Idealised CAD: detects any audible frame on air right now (hidden nodes still collide)."""
        for o in self.txs:
            if o.start <= self.now < o.end and (o.src == node or self.hears(node, o.src)):
                return True
        return False

    def cad_defer(self, key, node: str) -> bool:
        """True -> caller must retry later (already rescheduled by caller)."""
        if not self.cfg.cad:
            return False
        n = self.cad_tries.get(key, 0)
        if n < self.cfg.cad_max_tries and self.channel_busy(node):
            self.cad_tries[key] = n + 1
            self.stats["cad_backoffs"] += 1
            return True
        self.cad_tries.pop(key, None)
        return False

    def start_tx(self, src: str, kind: str, size_key: str, data: dict, reserved=False) -> Tx:
        dur = self.a[size_key]
        self.tx_id += 1
        tx = Tx(self.tx_id, src, self.now, self.now + dur, kind, data)
        self.txs.append(tx)
        if not reserved:
            self.busy[src].append((tx.start, tx.end))
        h = int(self.now // 3600)
        self.tx_time_bins[src][h] = self.tx_time_bins[src].get(h, 0.0) + dur
        self.push(tx.end, "tx_end", tx)
        return tx

    def prune(self):
        cut = self.now - 60
        if len(self.txs) > 400:
            self.txs = [x for x in self.txs if x.end >= cut]
        for n in self.busy:
            if len(self.busy[n]) > 50:
                self.busy[n] = [b for b in self.busy[n] if b[1] >= cut]

    def received(self, tx: Tx, rx: str) -> bool:
        """Pessimistic: receiver silent and no other audible overlapping frame."""
        if not self.hears(rx, tx.src):
            return False
        for o in self.txs:
            if o.id == tx.id:
                continue
            if o.start < tx.end and o.end > tx.start:
                if o.src == rx or self.hears(rx, o.src):
                    return False
        return True

    # ---------------- events ----------------
    def ev_sample(self, _):
        total = sum(len(t["queue"]) for t in self.trackers.values())
        mx = max(len(t["queue"]) for t in self.trackers.values())
        self.backlog_samples.append((self.now, total, mx))
        self.push(self.now + 60, "sample", None)

    def ev_report(self, tid):
        tr = self.trackers[tid]
        tr["seq"] += 1
        self.stats["records"] += 1
        tr["queue"].append({"seq": tr["seq"], "created": self.now})
        if len(tr["queue"]) > self.cfg.history_cap:
            tr["queue"].pop(0)
            self.stats["lost_capacity"] += 1
        j = self.cfg.jitter_frac * self.cfg.interval_s
        nxt = self.now + self.cfg.interval_s + self.rng.uniform(-j, j)
        self.push(nxt, "report", tid)
        if not tr["in_wake"]:
            q = tr["queue"]
            plan = [q[-1]["seq"]] + [r["seq"] for r in q[:-1]]   # live first, then oldest
            tr["plan"] = plan[: self.cfg.n_max]
            tr["in_wake"] = True
            tr["sent_in_wake"] = 0
            tr["retries_left"] = self.cfg.retry_in_wake
            self.push(self.now, "tracker_send", tid)

    def ev_tracker_send(self, tid):
        tr = self.trackers[tid]
        seqs = {r["seq"] for r in tr["queue"]}
        while tr["plan"] and tr["plan"][0] not in seqs:
            tr["plan"].pop(0)
        if not tr["plan"]:
            tr["in_wake"] = False
            return
        seq = tr["plan"].pop(0)
        s = self.free_at(tid, self.now, self.a["up"])
        if s > self.now + 1e-9:
            tr["plan"].insert(0, seq)
            self.push(s, "tracker_send", tid)
            return
        if self.cad_defer(("T", tid), tid):
            tr["plan"].insert(0, seq)
            self.push(self.now + self.rng.uniform(*self.cfg.cad_backoff), "tracker_send", tid)
            return
        self.stats["attempts"] += 1
        tx = self.start_tx(tid, "up", "up", {"dev": tid, "seq": seq, "relay_allowed": tr["relay_allowed"]})
        tr["tx_s"] += self.a["up"]
        win0 = tx.end + self.X - self.cfg.window_s / 2
        win1 = tx.end + self.X + self.cfg.window_s / 2
        tr["attempt"] = {"seq": seq, "tx_end": tx.end, "win": (win0, win1), "acked": False,
                         "relay_allowed": tr["relay_allowed"], "id": tx.id,
                         "online": self.gw_online(self.now)}
        tr["rx_s"] += self.cfg.window_s
        close = win1 + self.a["ackw"] + 0.05
        self.push(close, "window_close", (tid, tx.id))

    def ev_window_close(self, data):
        tid, att_id = data
        tr = self.trackers[tid]
        att = tr["attempt"]
        if not att or att["id"] != att_id or att["acked"]:
            return
        # miss: stop this wake, keep record, escalate route
        reason = att.get("miss_reason", "miss_no_uplink")
        self.stats[reason if att["online"] else "miss_outage"] += 1
        tr["relay_allowed"] = True if tr["hr"] else False
        tr["attempt"] = None
        if tr.get("retries_left", 0) > 0:
            tr["retries_left"] -= 1
            tr["plan"].insert(0, att["seq"])
            self.push(self.now + self.rng.uniform(*self.cfg.retry_backoff), "tracker_send", tid)
            return
        tr["in_wake"] = False

    def ev_tx_end(self, tx: Tx):
        self.prune()
        if tx.kind == "up":
            self.on_uplink_end(tx)
        elif tx.kind == "upw":
            if self.gw_online(tx.start) and self.gw_online(tx.end) and self.received(tx, "G"):
                self.gw_receive(tx.data, via_relay=True, rx_end=tx.end)
            else:
                self.stats["uplink_rx_fail_gw"] += 1
        elif tx.kind in ("ack", "ackw"):
            self.on_ack_end(tx)

    def on_uplink_end(self, tx: Tx):
        d = tx.data
        tr = self.trackers[d["dev"]]
        got_any = False
        if tr["hg"]:
            if self.gw_online(tx.start) and self.gw_online(tx.end) and self.received(tx, "G"):
                self.gw_receive(d, via_relay=False, rx_end=tx.end)
                got_any = True
            else:
                self.stats["uplink_rx_fail_gw"] += 1
        if tr["hr"] and d["relay_allowed"]:
            if self.received(tx, "R"):
                self.relay_receive(d, tx.end)
                got_any = True
            else:
                self.stats["uplink_rx_fail_relay"] += 1
        _ = got_any

    # ---------------- relay ----------------
    def relay_receive(self, d: dict, rx_end: float):
        key = (d["dev"], d["seq"])
        last = self.relay["dedupe"].get(key)
        if last is not None and rx_end - last < self.cfg.retry_dedupe_s:
            return
        self.relay["dedupe"][key] = rx_end
        if self.relay["pending"] >= self.cfg.relay_queue:
            self.stats["relay_queue_drop"] += 1
            return
        self.relay["pending"] += 1
        self.relay["hold"][key] = rx_end
        t = rx_end + self.rng.uniform(*self.cfg.relay_delay)
        self.push(t, "relay_forward", d)

    def ev_relay_forward(self, d):
        s = self.free_at("R", self.now, self.a["upw"])
        if s > self.now + 1e-9:
            self.push(s, "relay_forward", d)
            return
        if self.cad_defer(("R", d["dev"], d["seq"]), "R"):
            self.push(self.now + self.rng.uniform(*self.cfg.cad_backoff), "relay_forward", d)
            return
        self.relay["pending"] -= 1
        self.stats["relay_forwards"] += 1
        self.start_tx("R", "upw", "upw", dict(d))

    def relay_got_ack(self, d: dict):
        key = (d["dev"], d["seq"])
        rx_end = self.relay["hold"].pop(key, None)
        if rx_end is None:
            return
        slot = rx_end + self.X
        tr = self.trackers[d["dev"]]
        att = tr["attempt"]
        if slot < self.now + 0.02:
            if att and att["seq"] == d["seq"]:
                att["miss_reason"] = "miss_relay_late"
            return
        dur = self.a["ackw"]
        if self.overlaps_busy("R", slot, slot + dur):
            if att and att["seq"] == d["seq"]:
                att["miss_reason"] = "miss_slot_clash"
            return
        self.busy["R"].append((slot, slot + dur))
        self.push(slot, "relay_ack_tx", d)

    def ev_relay_ack_tx(self, d):
        self.start_tx("R", "ackw", "ackw", dict(d), reserved=True)

    # ---------------- gateway ----------------
    def gw_receive(self, d: dict, via_relay: bool, rx_end: float):
        key = (d["dev"], d["seq"])
        seen = self.gw["seen"].get(key)
        if seen and rx_end - seen["t"] < self.cfg.retry_dedupe_s + 10:
            self.stats["dup_copies_gw"] += 1
            if not via_relay and seen.get("direct_end") is None:
                seen["direct_end"] = rx_end
            return
        rec = {"t": rx_end, "direct_end": None if via_relay else rx_end}
        self.gw["seen"][key] = rec
        g = 0.0 if key in self.gw["committed"] else self.rng.uniform(*self.cfg.gw_commit)
        self.push(rx_end + g, "gw_commit", (d, rec))

    def ev_gw_commit(self, data):
        d, rec = data
        key = (d["dev"], d["seq"])
        if key not in self.gw["committed"]:
            self.gw["committed"].add(key)
            tr = self.trackers[d["dev"]]
            for r in tr["queue"]:
                if r["seq"] == d["seq"]:
                    self.latencies.append(self.now - r["created"])
                    break
        if not self.gw_online(self.now):
            return
        ack = {"dev": d["dev"], "seq": d["seq"]}
        if rec["direct_end"] is not None:
            slot = rec["direct_end"] + self.X
            dur = self.a["ack"]
            if slot < self.now + 0.02 or self.overlaps_busy("G", slot, slot + dur):
                tr = self.trackers[d["dev"]]
                att = tr["attempt"]
                if att and att["seq"] == d["seq"]:
                    att["miss_reason"] = "miss_slot_clash"
                return
            ack["relay_allowed"] = False
            self.busy["G"].append((slot, slot + dur))
            self.push(slot, "gw_ack_tx", ack)
        else:
            ack["relay_allowed"] = True
            self.push(self.now, "gw_ack_tx_asap", ack)

    def ev_gw_ack_tx(self, ack):
        self.stats["gw_acks"] += 1
        self.start_tx("G", "ack", "ack", ack, reserved=True)

    def ev_gw_ack_tx_asap(self, ack):
        s = self.free_at("G", self.now, self.a["ack"])
        if s > self.now + 1e-9:
            self.push(s, "gw_ack_tx_asap", ack)
            return
        if self.cad_defer(("G", ack["dev"], ack["seq"]), "G"):
            self.push(self.now + self.rng.uniform(0.1, 0.5), "gw_ack_tx_asap", ack)
            return
        self.stats["gw_acks"] += 1
        self.start_tx("G", "ack", "ack", ack)

    # ---------------- ACK reception ----------------
    def on_ack_end(self, tx: Tx):
        d = tx.data
        if tx.kind == "ack" and d.get("relay_allowed") and self.has_relay:
            if self.received(tx, "R"):
                self.relay_got_ack(d)
            else:
                tr = self.trackers[d["dev"]]
                att = tr["attempt"]
                if att and att["seq"] == d["seq"]:
                    att["miss_reason"] = "miss_ack_lost"
            return
        tid = d["dev"]
        tr = self.trackers[tid]
        att = tr["attempt"]
        if not att or att["seq"] != d["seq"] or att["acked"]:
            return
        w0, w1 = att["win"]
        if not (w0 <= tx.start <= w1):
            att["miss_reason"] = "miss_relay_late"
            return
        if not self.received(tx, tid):
            att["miss_reason"] = "miss_ack_lost"
            return
        # success
        att["acked"] = True
        tr["rx_s"] += tx.end - tx.start
        self.stats["acked"] += 1
        tr["queue"] = [r for r in tr["queue"] if r["seq"] != d["seq"]]
        tr["relay_allowed"] = (tx.kind == "ackw")
        tr["sent_in_wake"] += 1
        tr["attempt"] = None
        self.push(self.now + 0.1, "tracker_send", tid)

    # ---------------- results ----------------
    def results(self) -> dict:
        hours = self.cfg.hours

        def max_duty(node):
            bins = self.tx_time_bins.get(node, {})
            full = {h: v for h, v in bins.items() if h < int(hours)}
            return max(full.values()) / 3600 if full else 0.0

        tracker_duty = max((max_duty(t) for t in self.trackers), default=0.0)
        pending = sum(len(t["queue"]) for t in self.trackers.values())
        drain_s = None
        if self.cfg.outage:
            oe = self.cfg.outage[1] * 3600
            for (t, total, mx) in self.backlog_samples:
                if t >= oe and mx <= 1:
                    drain_s = t - oe
                    break
        n_tr = len(self.trackers)
        rx_per_h = statistics.mean(t["rx_s"] for t in self.trackers.values()) / hours
        tx_per_h = statistics.mean(t["tx_s"] for t in self.trackers.values()) / hours
        att = max(self.stats["attempts"] - self.stats["miss_outage"], 1)
        lat = sorted(self.latencies)
        return {
            **self.stats,
            "pending_end": pending,
            "delivered_unique": len(self.gw["committed"]),
            "ack_success_pct": 100 * self.stats["acked"] / att,
            "lat_p50_s": lat[len(lat) // 2] if lat else None,
            "lat_p95_s": lat[int(len(lat) * 0.95)] if lat else None,
            "max_backlog": max(m for (_, _, m) in self.backlog_samples),
            "drain_after_outage_s": drain_s,
            "duty_gw_pct": 100 * max_duty("G"),
            "duty_relay_pct": 100 * max_duty("R"),
            "duty_tracker_max_pct": 100 * tracker_duty,
            "tracker_rx_s_per_h": rx_per_h,
            "tracker_rx_s_per_h_10s_window": 10.0 * (self.stats["attempts"] / n_tr) / hours,
            "tracker_tx_s_per_h": tx_per_h,
            "X_s": self.X,
        }


def run_cfg(cfg: Config) -> dict:
    runs = [Sim(cfg, seed).run().results() for seed in range(cfg.seeds)]
    out = {}
    for k in runs[0]:
        vals = [r[k] for r in runs if r[k] is not None]
        if not vals:
            out[k] = None
        elif k in ("max_backlog", "drain_after_outage_s", "duty_gw_pct", "duty_relay_pct",
                   "duty_tracker_max_pct", "lat_p95_s"):
            out[k] = max(vals)             # worst seed for capacity-type metrics
        else:
            out[k] = statistics.mean(vals)
        if k == "drain_after_outage_s" and cfg.outage and len(vals) < len(runs):
            out[k] = None                  # at least one seed never drained
    return out


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------

def scenarios() -> list[Config]:
    out = [
        Config("S1", sf=11, groups={"direct": 1}),
        Config("S2", sf=11, groups={"relay": 1}),
        Config("S3", sf=11, groups={"both": 1}),
        Config("S4", sf=11, groups={"relay": 1}, outage=(6, 12)),
        Config("S5", sf=11, groups={"relay": 10}),
        Config("S6", sf=11, groups={"direct": 10, "both": 10, "relay": 10}, outage=(6, 12)),
        Config("S7", sf=9, groups={"direct": 10, "both": 10, "relay": 10}, outage=(6, 12)),
        Config("S8", sf=11, groups={"relay": 30}),
        Config("S9", sf=9, groups={"relay": 30}),
        Config("S5c", sf=11, groups={"relay": 10}, cad=True),
        Config("S6c", sf=11, groups={"direct": 10, "both": 10, "relay": 10}, outage=(6, 12), cad=True),
        Config("S7c", sf=9, groups={"direct": 10, "both": 10, "relay": 10}, outage=(6, 12), cad=True),
        Config("S9c", sf=9, groups={"relay": 30}, cad=True),
        Config("S10", sf=9, groups={"direct": 10, "both": 10, "relay": 10}, outage=(6, 12), cad=True,
               interval_s=60 * 60),
    ]
    return out


DESCR = {
    "S1": "1 tasma, gateway direkt, SF11",
    "S2": "1 tasma, sadece relay uzerinden, SF11",
    "S3": "1 tasma, hem gateway hem relay duyuyor, SF11",
    "S4": "1 tasma relay uzerinden, 6 saat gateway kapali, SF11",
    "S5": "10 tasma, hepsi relay uzerinden, SF11",
    "S6": "30 tasma karisik (10 direkt/10 ikisi/10 relay), 6 sa kesinti, SF11",
    "S7": "S6'nin aynisi, SF9",
    "S8": "30 tasma, hepsi tek relay uzerinden, SF11",
    "S9": "S8'in aynisi, SF9",
    "S5c": "S5 + CAD (gondermeden once kanali dinle)",
    "S6c": "S6 + CAD",
    "S7c": "S7 + CAD (SF9)",
    "S9c": "S9 + CAD (SF9)",
    "S10": "S7c ama tasmalar duruyor: 1 saatte 1 rapor",
}


def selftest() -> int:
    # ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md section 18 (SF11/BW125/CR4/5/preamble 8)
    ref = {34: 0.987136, 56: 1.396736, 72: 1.642496, 84: 1.888256, 96: 2.134016,
           100: 2.215936, 116: 2.461696}
    ok = True
    for b, t in ref.items():
        got = lora_airtime_s(b, 11)
        good = abs(got - t) < 1e-6
        ok &= good
        print(f"  {b:3d} B  ref {t:.6f}  sim {got:.6f}  {'OK' if good else 'FAIL'}")
    r = Sim(Config("selftest", groups={"direct": 1}, hours=6), 0).run().results()
    good = r["ack_success_pct"] == 100.0 and r["pending_end"] <= 1
    ok &= good
    print(f"  single direct tracker 6 h: ack {r['ack_success_pct']:.1f}%  {'OK' if good else 'FAIL'}")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def fmt(v, nd=1):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", help="run only this scenario id (S1..S9)")
    ap.add_argument("--json", help="write full results to this JSON file")
    ap.add_argument("--seeds", type=int, default=None)
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="override a Config field for the selected scenarios, e.g. --set sf=9 --set n_max=8 --set cad=1")
    ap.add_argument("--selftest", action="store_true", help="check airtime against the SF5B reference table and exit")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()

    print("Airtime (s), BW125 CR4/5 preamble 8:")
    for sf in (8, 9, 10, 11):
        c = Config(sf=sf)
        a = c.airtimes()
        print(f"  SF{sf}: PERIODIC {a['up']:.3f}  wrapped {a['upw']:.3f}  "
              f"ACK {a['ack']:.3f}  wrapped ACK {a['ackw']:.3f}  -> X_min {c.x_min():.2f}  X {c.x():.0f}")
    print()

    results = {}
    for cfg in scenarios():
        if args.scenario and cfg.name != args.scenario:
            continue
        if args.seeds:
            cfg.seeds = args.seeds
        for kv in args.set:
            k, v = kv.split("=", 1)
            cur = getattr(cfg, k)
            if isinstance(cur, bool):
                v = v.lower() in ("1", "true", "yes")
            elif isinstance(cur, int):
                v = int(v)
            elif isinstance(cur, float) or cur is None:
                v = float(v)
            setattr(cfg, k, v)
        r = run_cfg(cfg)
        results[cfg.name] = {"description": DESCR[cfg.name], "config": {
            "sf": cfg.sf, "groups": cfg.groups, "outage_h": cfg.outage, "interval_s": cfg.interval_s,
            "n_max": cfg.n_max, "window_s": cfg.window_s, "X_s": cfg.x(), "hours": cfg.hours,
            "seeds": cfg.seeds, "cad": cfg.cad}, "results": r}
        lim = 100 * cfg.duty_limit
        flag = lambda d: " !!" if d > lim else ""
        print(f"{cfg.name}  {DESCR[cfg.name]}   (X={cfg.x():.0f}s)")
        print(f"   ACK basari (gateway acikken) {fmt(r['ack_success_pct'])}%  | gecikme p50 {fmt(r['lat_p50_s'],0)}s "
              f"p95 {fmt(r['lat_p95_s'],0)}s | kayip(kapasite) {fmt(r['lost_capacity'],0)} "
              f"| sonda bekleyen {fmt(r['pending_end'],1)}")
        print(f"   kacan ACK: uplink yok {fmt(r['miss_no_uplink'],1)}, relay gec {fmt(r['miss_relay_late'],1)}, "
              f"slot cakismasi {fmt(r['miss_slot_clash'],1)}, ACK bozuldu {fmt(r['miss_ack_lost'],1)} "
              f"| relay kuyruk dusurme {fmt(r['relay_queue_drop'],1)}"
              + (f" | CAD bekleme {fmt(r['cad_backoffs'],0)}" if cfg.cad else "")
              + (f" | kesintide bosa deneme {fmt(r['miss_outage'],0)}" if cfg.outage else ""))
        dr = r["drain_after_outage_s"]
        print(f"   max backlog {fmt(r['max_backlog'],0)} kayit"
              + (f" | kesinti sonrasi bosalma {fmt(dr/60 if dr is not None else None,0)} dk" if cfg.outage else ""))
        print(f"   duty (en kotu saat): gateway {fmt(r['duty_gw_pct'])}%{flag(r['duty_gw_pct'])}  "
              f"relay {fmt(r['duty_relay_pct'])}%{flag(r['duty_relay_pct'])}  "
              f"tasma {fmt(r['duty_tracker_max_pct'])}%{flag(r['duty_tracker_max_pct'])}")
        print(f"   tasma dinleme: {fmt(r['tracker_rx_s_per_h'])} s/saat "
              f"(bugunku 10 s pencereyle {fmt(r['tracker_rx_s_per_h_10s_window'])} s/saat)")
        print()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
