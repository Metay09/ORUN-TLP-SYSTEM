#!/usr/bin/env python3
"""SF5L6: PC/phone side of the BLE Edge link (host only, no radio).

- Framing equals the firmware's (shared vector with test_sf5l6_ble_edge.cpp)
  and round-trips every size; broken sequences are dropped.
- Gateway messages drive the same ledger + accept queue as USB: HELLO sets
  the gateway id, HELD is verified then accepted (binary ACCEPT), the RX
  link becomes the record's signal, ACCEPT_RESULT/RECLAIMED/DUMP_END update.
"""
from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orun_edge_bench as eb  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

GW = 0x09A462BD4B275BA5


def held_msg(page, slot, gen, obj, link=None) -> bytes:
    flags, rssi, snr = (1, link[0], link[1]) if link else (0, 0, 0)
    return (bytes([0x02]) + page.to_bytes(2, "big") + slot.to_bytes(2, "big") +
            gen.to_bytes(8, "big") + bytes([flags]) +
            rssi.to_bytes(2, "big", signed=True) + snr.to_bytes(1, "big", signed=True) +
            bytes([len(obj)]) + obj)


def via_frames(msg: bytes, mid: int = 3) -> bytes:
    r = eb.EdgeReassembler()
    out = None
    for f in eb.edge_frames(msg, mid):
        assert len(f) <= 20
        out = r.push(f)
    return out


def main() -> None:
    frames = eb.edge_frames(bytes(range(40)), 9)
    assert [f.hex().upper() for f in frames] == [
        "01010900000102030405060708090A0B0C0D0E0F",
        "01000901101112131415161718191A1B1C1D1E1F",
        "010209022021222324252627"]
    for n in range(1, eb.EDGE_MAX_MESSAGE + 1):
        msg = bytes((i * 7 + 3) & 0xFF for i in range(n))
        assert via_frames(msg) == msg
    r = eb.EdgeReassembler()
    assert r.push(frames[0]) is None
    assert r.push(frames[2]) is None and not r.active      # gap -> dropped
    assert r.push(frames[1]) is None                        # no start
    try:
        eb.edge_frames(bytes(161), 1)
        raise AssertionError("oversize accepted")
    except ValueError:
        pass

    track = l3.track(3)
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        edge = eb.BenchEdge(db, test_key=True)
        # HELD before HELLO: the gateway id is unknown, never guessed.
        assert edge.handle_ble(via_frames(held_msg(0, 0, 1, track[0]))) is None
        hello = bytes([0x01]) + GW.to_bytes(8, "big") + (5).to_bytes(2, "big") + (1056).to_bytes(2, "big")
        out = edge.handle_ble(via_frames(hello))
        assert "09A462BD4B275BA5" in out and "5/1056" in out
        out = edge.handle_ble(via_frames(held_msg(0, 0, 1, track[0], link=(-97, 7))))
        assert out.startswith("YENİ") and "sinyal -97 dBm SNR 7" in out
        acc = edge.next_accept_binary(10.0)
        assert acc == eb.edge_accept_binary(GW, 0, 0, 1, track[0])
        assert len(acc) == 45 and acc[0] == 0x82
        assert via_frames(acc) == acc and len(eb.edge_frames(acc, 1)) == 3
        assert edge.next_accept_binary(10.5) is None          # one in flight
        # BUSY -> resent; HANDED_OFF -> done.
        assert edge.handle_ble(bytes([0x04, 0, 0, 0, 0, 3])) is None
        assert edge.next_accept_binary(10.6) == acc
        assert edge.handle_ble(bytes([0x04, 0, 0, 0, 0, 0])) is None
        assert edge.handed_off == 1 and edge.pending_accepts() == 0
        # Rejected (bad MAC) is reported, not retried.
        edge.handle_ble(via_frames(held_msg(0, 1, 1, track[1])))
        assert edge.next_accept_binary(20.0) is not None
        warn = edge.handle_ble(bytes([0x04, 0, 0, 0, 1, 4]))
        assert warn and "reddetti" in warn and edge.next_accept_binary(30.0) is None
        assert "boşaltıldı" in edge.handle_ble(bytes([0x05, 0, 2, 1]))
        assert "silinemedi" in edge.handle_ble(bytes([0x05, 0, 2, 0]))
        assert "DUMP END count=2" in edge.handle_ble(bytes([0x03, 0, 2]))
        # The RX link from HELD is the record's signal, like the USB RX line.
        with sqlite3.connect(str(db)) as c:
            rssi = c.execute("SELECT gw_rssi, gw_snr FROM edge_receipts WHERE gw_rssi IS NOT NULL").fetchall()
        assert rssi == [(-97, 7)]
        # Unknown / malformed messages are ignored.
        assert edge.handle_ble(b"") is None and edge.handle_ble(bytes([0x7F])) is None

    print("SF5L6 BLE Edge (PC side): framing vector, reassembly, HELLO/HELD/"
          "accept/result/reclaim/dump-end, RX link: PASS (host only)")


if __name__ == "__main__":
    main()
