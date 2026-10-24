#!/usr/bin/env python3
"""SF5L4: range-test kit, host only.

- The Edge tool pairs the Gateway's own RX line (RSSI/SNR) with the record
  that line's custody write produced, and never lends it to another record.
- First PC arrival time is kept; a later DUMP is not a new arrival.
- /api/points exposes rssi/snr/delay/period and the PC clock.
- The map page carries the range view and the freshness of the NEWEST
  position (by record time, not by arrival).
- The live-first TX order is a bench-only switch in main.cpp.
"""
from __future__ import annotations

import datetime as dt
from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orun_edge_bench as eb  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

RX = "RX PRODUCT_SECURE queued family=1 length=100 rssi=%d snr=%d unverified=yes"
STARTED = "BENCH GW RX record; durable custody write started"
DUP = "BENCH GW RX duplicate already held; ACK deferred"


def held(i: int, frame: bytes) -> str:
    return ("EDGE HELD v1 gw=09A462BD4B275BA5 page=0 slot=%d "
            "gen=0000000000000001 len=100 obj=%s" % (i, frame.hex().upper()))


def record_time(frame: bytes, edge: eb.BenchEdge) -> float:
    item = edge.ledger_mod.open_product_frame(frame, edge.creds)
    iso = eb.describe_periodic(item.device_id, item.plaintext)["time"]
    return dt.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()


def tool() -> None:
    frames = l3.track(4)
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        now = [0.0]
        edge = eb.BenchEdge(db, test_key=True, clock=lambda: now[0])
        t0 = record_time(frames[0], edge)

        # Live record: RX -> custody started -> EDGE HELD, 30 s after capture.
        now[0] = t0 + 30
        assert edge.handle_line(RX % (-97, 8)) is None
        assert edge.handle_line(STARTED) is None
        out = edge.handle_line(held(0, frames[0]))
        assert out.startswith("YENİ") and "sinyal -97 dBm SNR 8" in out, out
        assert "\n    https://maps.google.com" in out  # link stays on its own line

        # A duplicate RX must not lend its RSSI to the next DUMP line.
        assert edge.handle_line(RX % (-80, 10)) is None
        assert edge.handle_line(DUP) is None
        now[0] = record_time(frames[1], edge) + 3600   # backlog, an hour late
        out = edge.handle_line(held(1, frames[1]))
        assert out.startswith("YENİ") and "sinyal" not in out, out

        # An RX without a custody write (e.g. rejected) is dropped as well.
        assert edge.handle_line(RX % (-70, 11)) is None
        now[0] = record_time(frames[2], edge) + 10
        assert "sinyal" not in edge.handle_line(held(2, frames[2]))

        # A later DUMP of the live record keeps first arrival and its RSSI.
        now[0] = t0 + 9999
        out = edge.handle_line(held(0, frames[0]))
        assert out.startswith("VARDI") and "sinyal" not in out, out

        view = eb.points_from_db(db)
        assert view["count"] == 3 and view["now"].endswith("Z")
        p = {x["seq"]: x for x in view["points"]}
        assert (p[1000]["rssi"], p[1000]["snr"]) == (-97, 8)
        assert p[1000]["delay_s"] == 30 and p[1000]["period_s"] > 0
        assert p[1000]["received"].endswith("Z")
        assert p[1001]["rssi"] is None and p[1001]["delay_s"] == 3600
        assert p[1002]["rssi"] is None and p[1002]["delay_s"] == 10

        # Ledgers written before SF5L4 (no receipts table) still map.
        old = Path(tmp) / "old.sqlite"
        legacy = edge.ledger_mod.VerifiedLocalLedger(old, edge.creds)
        assert legacy.ingest(frames[3]).state.name == "VERIFIED_STORED"
        legacy_view = eb.points_from_db(old)
        assert legacy_view["count"] == 1
        assert legacy_view["points"][0]["rssi"] is None
        assert legacy_view["points"][0]["delay_s"] is None


def links() -> None:
    """Coverage evidence: queue delay is not a missing link."""
    T = 1_800_000_000

    def pt(i, delay, rssi=None, tracker="A"):
        iso = dt.datetime.fromtimestamp(T + 60 * i, tz=dt.timezone.utc).isoformat()
        return {"tracker": tracker, "time": iso.replace("+00:00", "Z"),
                "received": "x", "delay_s": delay, "rssi": rssi}

    session = [(T - 10, T + 60 * 30)]
    pts = {
        "live": pt(0, 5, -90),
        "queued": pt(1, 3600),          # live record 60 s before it: link was up
        "through": pt(2, 60),
        "cut": pt(5, 3600),             # nearest live capture is >150 s away
        "live2": pt(8, 7, -120),
        "slow_live": pt(9, 45, -100),   # RSSI known but too late to place
        "other_tracker": pt(5, 3600, tracker="B"),
        "unreceived": dict(pt(6, None), received=None),
    }
    eb.classify_links(list(pts.values()), session)
    got = {k: v["link"] for k, v in pts.items()}
    assert got == {"live": "live", "queued": None, "through": "through",
                   "cut": "cut", "live2": "live", "slow_live": "through",
                   "other_tracker": "cut", "unreceived": None}, got
    # Captured while nobody listened (e.g. the 20 October backlog): unknown.
    late = [pt(5, 3600)]
    eb.classify_links(late, [(T + 60 * 10, T + 60 * 30)])
    assert late[0]["link"] is None
    eb.classify_links(late, [])
    assert late[0]["link"] is None

    # The tool records its attachment to a Gateway; the API classifies.
    frames = l3.track(3)
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        now = [0.0]
        edge = eb.BenchEdge(db, test_key=True, clock=lambda: now[0])
        t0 = record_time(frames[0], edge)
        now[0] = t0 + 60 * 3        # attached only after frame 0/1/2 capture
        edge.session_alive()
        for i, f in enumerate(frames):
            edge.handle_line(held(i, f))
        now[0] += 15
        edge.session_alive()
        view = eb.points_from_db(db)
        assert [p["link"] for p in view["points"]] == [None, None, None]
        with sqlite3.connect(str(db)) as c:
            assert c.execute("SELECT COUNT(*), MIN(started_utc), MAX(alive_utc) "
                             "FROM edge_sessions").fetchone() == (
                                 1, int(t0 + 180), int(t0 + 195))


def page() -> None:
    html = (ROOT / "tools/edge_map/index.html").read_text()
    for needle in ('data-view="range"', 'data-view="grid"', "rssiColor",
                   "freshness(", "ESKİ", "geç geldi", "serverOffset", "data.now",
                   "[3, 5, 10, 20]", "let cellM = 5;", "p.link && GOOD_FIX(p)",
                   "SNR_FLOOR = -17.5", "GOOD_FIX", "median("):
        assert needle in html, needle
    assert "innerHTML" not in html.replace("outerHTML", "")
    # The big marker is the newest by RECORD time (sorted by time, then seq).
    assert 'list.sort((a, b) => (a.time || "").localeCompare(b.time || "") || a.seq - b.seq)' in html
    assert "const last = list[list.length - 1];" in html


def firmware() -> None:
    main = (ROOT / "firmware/src/main.cpp").read_text()
    at = main.index("tracker_product_tx.setLiveFirst(true)")
    assert main.count("setLiveFirst(") == 1
    opened = main.rfind("#ifdef ORUN_SF5L0_TRACKER", 0, at)
    assert opened >= 0 and "#endif" not in main[opened:at]
    for line in ("BENCH LINK tx seq=", "BENCH LINK ack seq=",
                 "BENCH LINK ack_timeout seq=", '"BENCH DRAIN LIVE", 16',
                 "tracker_product_tx.setLiveOnly(true);",
                 "tracker_product_tx.setLiveOnly(false);"):
        assert line in main, line
    pilot = (ROOT / "firmware/include/tracker_product_tx_pilot.h").read_text()
    assert "bool live_first_ = false;" in pilot  # default stays SF5K3V
    assert "bool live_only_ = false;" in pilot


def main() -> None:
    tool()
    links()
    page()
    firmware()
    print("SF5L4 range kit: RSSI pairing, arrival/delay, map range view, "
          "coverage link classes, bench-only live-first: PASS (host only)")


if __name__ == "__main__":
    main()
