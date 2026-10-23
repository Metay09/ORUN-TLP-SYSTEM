#!/usr/bin/env python3
"""SF5L1: tools/orun_edge_bench.py against the SF5L0 Gateway USB line format.

Uses the SF5B golden frames (the on-device CC310 KAT periodic frame and the
host periodic frame), both sealed with the same PUBLIC test credential the
SF5L0 bench images use. Checks: line parsing, AEAD open through the existing
backend ledger, readable decode, dedupe on repeat, refusal of tampered frames
and of everything when the test key is not enabled. Host only.
"""
from __future__ import annotations

import io
from pathlib import Path
import re
import sys
import tempfile
from contextlib import redirect_stdout

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import orun_edge_bench as eb  # noqa: E402


def fixture(name: str) -> bytes:
    text = (ROOT / "firmware/tests/sf5/sf5b_product_vectors.h").read_text()
    m = re.search(r"static const char " + re.escape(name) +
                  r"\[\]\s*=\s*((?:\s*\"[0-9a-f]+\"\s*)+);", text)
    assert m is not None, name
    return bytes.fromhex("".join(re.findall(r'"([0-9a-f]+)"', m.group(1))))


def line(obj: bytes, slot: int = 0) -> str:
    # Exactly the printf format in firmware/src/sf5l0_bench_gateway.cpp.
    return ("EDGE HELD v1 gw=09A462BD4B275BA5 page=0 slot=%u "
            "gen=0000000000000001 len=%u obj=%s" % (slot, len(obj), obj.hex().upper()))


def main() -> None:
    src = (ROOT / "firmware/src/sf5l0_bench_gateway.cpp").read_text()
    assert 'printf("EDGE HELD v1 gw=%s page=%u slot=%u gen=%s len=%u obj=%s\\n"' in src
    assert eb.TEST_CREDENTIAL_ID == bytes(range(0xA0, 0xB0))
    assert eb.TEST_ROOT == bytes(range(32)) and eb.TEST_KEY_EPOCH == 0x01020304

    kat = fixture("kKatPeriodicFrameHex")   # produced by real RAK4631 crypto
    host = fixture("kPeriodicFrameHex")
    assert len(kat) == 100 and len(host) == 100

    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        edge = eb.BenchEdge(db, test_key=True)

        assert edge.handle_line("RX PRODUCT_SECURE queued ...") is None
        out = edge.handle_line(line(kat))
        assert out and out.startswith("YENİ") and "tasma=1122334455667788" in out, out
        assert "konum=" in out and "pil=" in out, out
        # Same logical record (same incarnation/sequence) re-sealed with a
        # different counter: recognised as the same observation, not doubled.
        out2 = edge.handle_line(line(host, 1))
        assert out2 and out2.startswith("VARDI"), out2

        # The same exact frame again (DUMP after a live line): not doubled.
        again = edge.handle_line(line(kat))
        assert again and again.startswith("VARDI"), again
        assert edge.ledger.record_count() == 1

        # One flipped bit anywhere: AEAD refuses, nothing stored.
        bad = bytearray(kat)
        bad[60] ^= 1
        assert edge.handle_line(line(bytes(bad))).startswith("RED")
        short = line(kat).replace("len=100", "len=84")
        assert edge.handle_line(short).startswith("RED")
        assert edge.ledger.record_count() == 1
        assert edge.counts["STORED"] == 1 and edge.counts["DUPLICATE"] == 2

        # Readable decode is consistent with the canonical plaintext.
        item = edge.ledger_mod.open_product_frame(kat, edge.creds)
        d = eb.describe_periodic(item.device_id, item.plaintext)
        assert d["tracker"] == "1122334455667788" and d["sequence"] > 0
        assert d["lat"] is not None and -90 <= d["lat"] <= 90
        assert d["lon"] is not None and -180 <= d["lon"] <= 180

        # --list reads back from the SQLite file, not RAM.
        buf = io.StringIO()
        with redirect_stdout(buf):
            eb.list_db(db, True)
        assert "toplam=1" in buf.getvalue()

        # Without the explicit bench flag no credential exists: all refused.
        locked = eb.BenchEdge(Path(tmp) / "locked.sqlite", test_key=False)
        assert locked.handle_line(line(kat)).startswith("RED")
        assert locked.ledger.record_count() == 0

        # --file path end to end.
        log = Path(tmp) / "gw.log"
        log.write_text("boot noise\n" + line(kat) + "\n" + line(host, 1) + "\n"
                       "EDGE DUMP END count=2\n")
        buf = io.StringIO()
        with redirect_stdout(buf):
            assert eb.main(["--bench-test-key", "--file", str(log),
                            "--db", str(Path(tmp) / "file.sqlite")]) == 0
        text = buf.getvalue()
        assert text.count("YENİ") == 1 and text.count("VARDI") == 1, text
        assert "toplam_kayıt=1" in text, text

    print("SF5L1 bench Edge USB line -> verified ledger: PASS (host only)")


if __name__ == "__main__":
    main()
