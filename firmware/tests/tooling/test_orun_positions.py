"""tools/orun_positions.py against the BRIDGE_FRAME_V1.md reference lines."""

import contextlib
import io
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import orun_positions as op  # noqa: E402

# Same golden lines as firmware/tests/m5/test_bridge_frame.cpp.
DIRECT = ("BRIDGE v=1 kind=POSITION n=1 up=123456 node=0102030405060708 dup=0 "
          "path=DIRECT rssi=-82 snr=6 "
          "raw=010289ABCDEF01234567000000286553F10018701A8011490C800000303900AF0907 "
          "crc=8E07")
RELAY = ("BRIDGE v=1 kind=POSITION n=4294967295 up=4294967295 "
         "node=0123456789ABCDEF dup=1 path=RELAY relay=FEDCBA9876543210 "
         "in_rssi=-32768 in_snr=-128 rssi=-32768 snr=-128 "
         "raw=0102FFFFFFFFFFFFFFFFFFFFFFFF00000000CA5B170094B62E0080000000FFFFFF00 "
         "crc=792F")


def decode():
    assert op.crc16("123456789") == 0x29B1
    p = op.parse_line(DIRECT + "\r\n")
    assert p["source"] == "89ABCDEF01234567" and p["sequence"] == 40
    assert p["utc"] == 1700000000 and p["flags"] == 0x07
    assert p["lat_e7"] == 410000000 and p["lon_e7"] == 290000000
    assert p["altitude_mm"] == 12345 and p["hdop_x100"] == 175
    assert p["satellites"] == 9 and not p["duplicate"]
    line = op.readable(p)
    assert line.startswith("2023-11-14 22:13:20 UTC  tasma=89ABCDEF01234567 sira=40")
    assert "41.0000000,29.0000000  uydu=9 hdop=1.75" in line
    assert line.endswith("https://maps.google.com/?q=41.0000000,29.0000000")

    r = op.parse_line(RELAY)
    assert r["duplicate"] and r["path"] == "RELAY" and r["flags"] == 0
    assert r["lat_e7"] == -900000000 and r["lon_e7"] == -1800000000
    assert "konum yok" in op.readable(r) and "maps" not in op.readable(r)
    assert "saat yok" in op.readable(r)
    assert op.degrees(-5) == "-0.0000005"

    # A logger prefix before the marker is kept as receipt text.
    prefixed = op.parse_line("[2026-10-11 18:00:00] " + DIRECT)
    assert prefixed["received"] == "[2026-10-11 18:00:00]"


def rejects():
    assert op.parse_line("BASE RX NEW source=89ABCDEF01234567 seq=40") is None
    assert op.parse_line(DIRECT.replace("crc=8E07", "crc=8E08")) is None
    assert op.parse_line(DIRECT.replace("rssi=-82", "rssi=-83")) is None
    assert op.parse_line(DIRECT.replace("BRIDGE v=1", "BRIDGE v=2")) is None
    assert op.parse_line(DIRECT[:-9]) is None


def cli():
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, "alici.log")
        track = os.path.join(tmp, "iz.kml")
        with open(log, "w") as handle:
            handle.write("GNSS noise\n" + DIRECT + "\n" + DIRECT + "\n" +
                         RELAY + "\n")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            assert op.main([log, "--kml", track]) == 0
        lines = out.getvalue().splitlines()
        assert len(lines) == 2  # the repeated DIRECT copy is skipped
        assert "KML:" in err.getvalue() and "(1 konum)" in err.getvalue()
        with open(track) as handle:
            text = handle.read()
        assert "<coordinates>29.0000000,41.0000000</coordinates>" in text
        assert "LineString" not in text  # one fix, no track line
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            op.main([log, "--all"])
        assert len(out.getvalue().splitlines()) == 3


decode()
rejects()
cli()
print("Receiver log position decoder checks: PASS")
