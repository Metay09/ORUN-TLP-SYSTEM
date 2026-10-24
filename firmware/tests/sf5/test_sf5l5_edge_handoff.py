#!/usr/bin/env python3
"""SF5L5: PC side of the bench Edge hand-off (host only).

- The EDGE ACCEPT MAC equals the firmware's (shared vector with
  test_sf5l5_edge_handoff.cpp).
- An accept is queued only for a record the ledger durably verified
  (STORED or DUPLICATE), one in flight at a time; BUSY and silence are
  retried, REJECTED is reported and not retried.
"""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orun_edge_bench as eb  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

GW = 0x09A462BD4B275BA5
# makeFrame(kTracker, test epoch, counter 1) from the C++ test.
VECTOR_OBJECT = bytes.fromhex("02040201030140000E8ADE7E71531AA30102030400000000000000010104070A0D101316191C1F2225282B2E3134373A3D404346494C4F5255585B5E6164676A6D707376797C7F8285888B8E9194979A9DA0A3A6A9ACAFB2B5B8BBBE5051525354555657")
VECTOR_MAC = "EE0E6E26790F83C816F6738CF71DF64B900A659F00E65C9DAD8C92B2E486C521"


def held(page: int, slot: int, gen: int, frame: bytes) -> str:
    return (f"EDGE HELD v1 gw={GW:016X} page={page} slot={slot} gen={gen:016X} "
            f"len={len(frame)} obj={frame.hex().upper()}")


def main() -> None:
    assert eb.edge_accept_mac(GW, 1, 2, 3, VECTOR_OBJECT).hex().upper() == VECTOR_MAC
    line = eb.edge_accept_line(GW, 1, 2, 3, VECTOR_OBJECT)
    assert line == ("EDGE ACCEPT v1 page=1 slot=2 gen=0000000000000003 mac=" + VECTOR_MAC)
    assert len(line) < 160  # the Gateway's bench command buffer

    frames = l3.track(3)
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        edge = eb.BenchEdge(db, test_key=True)
        assert edge.handle_line(held(4, 0, 9, frames[0])).startswith("YENİ")
        t = 100.0
        sent = edge.next_accept(t)
        assert sent == eb.edge_accept_line(GW, 4, 0, 9, frames[0])
        assert edge.next_accept(t + 0.5) is None          # one in flight
        # Store busy: the same accept goes again.
        assert edge.handle_line("EDGE ACCEPT BUSY page=4 slot=0") is None
        assert edge.next_accept(t + 0.6) == sent
        # Silence: resent after the timeout.
        assert edge.next_accept(t + 0.6 + eb.ACCEPT_TIMEOUT_S + 0.1) == sent
        assert edge.handle_line("EDGE ACCEPT OK page=4 slot=0 state=HANDED_OFF") is None
        assert edge.handed_off == 1 and edge.pending_accepts() == 0

        # A record already in the ledger (DUMP repeat) is accepted again.
        assert edge.handle_line(held(4, 0, 9, frames[0])).startswith("VARDI")
        assert edge.next_accept(t + 10) == sent
        assert edge.handle_line("EDGE ACCEPT OK page=4 slot=0 state=ALREADY") is None

        # REJECTED: reported once, not retried.
        edge.handle_line(held(4, 1, 9, frames[1]))
        assert edge.next_accept(t + 20) is not None
        # Exactly what the Gateway prints (sf5l0_bench_gateway.cpp edgeAccept).
        warn = edge.handle_line("EDGE ACCEPT REJECTED reason=MAC page=4 slot=1")
        assert warn and "reddetti" in warn and edge.accept_rejected == 1
        assert edge.next_accept(t + 30) is None
        for reason in ("MALFORMED", "STORE"):
            edge.handle_line(held(4, 1, 9, frames[1]))
            assert edge.next_accept(t + 31) is not None
            assert "reddetti" in edge.handle_line(
                f"EDGE ACCEPT REJECTED reason={reason} page=4 slot=1")
            assert edge.next_accept(t + 40) is None

        # A frame the ledger cannot verify is never accepted.
        bad = bytearray(frames[2]); bad[-1] ^= 1
        assert edge.handle_line(held(4, 2, 9, bytes(bad))).startswith("RED")
        assert edge.next_accept(t + 40) is None

        # Reclaim lines are reported.
        assert "boşaltıldı" in edge.handle_line("EDGE RECLAIM page=4 OK")
        assert "teslim=" in edge.summary()

        # --no-accept: nothing is ever sent.
        quiet = eb.BenchEdge(Path(tmp) / "q.sqlite", test_key=True, accept=False)
        quiet.handle_line(held(1, 0, 1, frames[0]))
        assert quiet.next_accept(0.0) is None and "teslim=" not in quiet.summary()

    print("SF5L5 Edge accept: MAC vector, durable-only queue, retry/timeout, "
          "reject, no-accept: PASS (host only)")


if __name__ == "__main__":
    main()
