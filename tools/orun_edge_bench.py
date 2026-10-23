#!/usr/bin/env python3
"""SF5L1 bench Edge: Gateway USB hand-out -> verified local ledger.

Reads `EDGE HELD v1 ...` lines from an SF5L0 bench Gateway (USB serial or a
saved log), opens each exact PRODUCT_SECURE frame with the existing backend
candidate (backend/candidates/product_secure_verified_ledger.py: AES-CCM
authentication + canonical-content checks + SQLite dedupe) and prints the
record in readable form.

BENCH ONLY: the only credential this tool knows is the PUBLIC SF5L0 test
credential, enabled by --bench-test-key. It never sends anything that changes
the Gateway; it only asks for `BENCH GW DUMP` (a read-only listing).

Needs: python3 -m pip install cryptography pyserial   (pyserial only for --port)

Examples:
  python3 tools/orun_edge_bench.py --bench-test-key --port /dev/ttyACM0
  python3 tools/orun_edge_bench.py --bench-test-key --file gateway.log
  python3 tools/orun_edge_bench.py --list
"""
from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import os
from pathlib import Path
import re
import sqlite3
import sys
import time
from typing import Iterable, Optional

ROOT = Path(__file__).resolve().parents[1]
LEDGER_PATH = ROOT / "backend" / "candidates" / "product_secure_verified_ledger.py"

# Same public vector as firmware/src/sf5l0_bench.cpp (and the SF3 qualification).
TEST_CREDENTIAL_ID = bytes(range(0xA0, 0xB0))
TEST_ROOT = bytes(range(0x00, 0x20))
TEST_KEY_EPOCH = 0x01020304

LINE = re.compile(
    r"EDGE HELD v1 gw=(?P<gw>[0-9A-F]{16}) page=(?P<page>\d+) slot=(?P<slot>\d+) "
    r"gen=(?P<gen>[0-9A-F]{16}) len=(?P<len>\d+) obj=(?P<obj>[0-9A-F]+)")

SOURCES = {0: "UNKNOWN", 1: "GNSS", 2: "FIXED", 3: "PHONE", 4: "MANUAL"}
BATTERY = {0: "UNKNOWN", 1: "NORMAL", 2: "LOW", 3: "CRITICAL"}


def load_ledger_module():
    spec = importlib.util.spec_from_file_location("orun_ledger", LEDGER_PATH)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {LEDGER_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolve their module here
    spec.loader.exec_module(module)
    return module


def bench_credentials(ledger):
    def lookup(device: bytes, epoch: int):
        if epoch != TEST_KEY_EPOCH:
            return None
        return ledger.BackendCredential(TEST_CREDENTIAL_ID, TEST_ROOT)
    return lookup


def no_credentials(_device: bytes, _epoch: int):
    return None


def _u(b: bytes) -> int:
    return int.from_bytes(b, "big")


def _i(b: bytes) -> int:
    return int.from_bytes(b, "big", signed=True)


def describe_periodic(device: bytes, p: bytes) -> dict:
    """Readable fields of a canonical SF5B PERIODIC plaintext (64 bytes)."""
    flags = p[1]
    out = {
        "tracker": device.hex().upper(),
        "incarnation": p[4:12].hex().upper(),
        "sequence": _u(p[12:16]),
        "period_s": _u(p[20:24]),
        "time": None, "lat": None, "lon": None, "sats": None, "hdop": None,
        "fix_age_s": None, "battery_mv": None, "battery": None,
        "active_s": None, "covered_s": None, "source": SOURCES.get(p[2], "?"),
    }
    if flags & 1:
        out["time"] = dt.datetime.fromtimestamp(
            _u(p[16:20]), tz=dt.timezone.utc).isoformat().replace("+00:00", "Z")
    if flags & 2:
        out["lat"] = _i(p[24:28]) / 1e7
        out["lon"] = _i(p[28:32]) / 1e7
        if p[2] == 1:
            out["sats"] = p[40]
            out["hdop"] = _u(p[36:38]) / 100
        if flags & 8:
            out["fix_age_s"] = _u(p[38:40])
    if flags & 16:
        out["covered_s"] = _u(p[44:48])
        out["active_s"] = _u(p[48:52])
    if flags & 32:
        out["battery_mv"] = _u(p[56:58])
        out["battery"] = BATTERY.get(p[3], "?")
    return out


def human(d: dict) -> str:
    parts = [f"tasma={d['tracker']}", f"seq={d['sequence']}"]
    parts.append(f"zaman={d['time'] or 'bilinmiyor'}")
    if d["lat"] is not None:
        q = []
        if d["sats"] is not None:
            q.append(f"uydu={d['sats']}")
        if d["hdop"]:
            q.append(f"hdop={d['hdop']:.2f}")
        parts.append(f"konum={d['lat']:.6f},{d['lon']:.6f}" +
                     (f" ({' '.join(q)})" if q else ""))
    else:
        parts.append("konum=YOK")
    if d["battery_mv"] is not None:
        parts.append(f"pil={d['battery_mv']}mV {d['battery']}")
    if d["active_s"] is not None:
        parts.append(f"aktif={d['active_s']}/{d['covered_s']}s")
    line = " ".join(parts)
    if d["lat"] is not None:
        line += f"\n    https://maps.google.com/?q={d['lat']:.6f},{d['lon']:.6f}"
    return line


class BenchEdge:
    def __init__(self, db_path: Path, test_key: bool):
        self.ledger_mod = load_ledger_module()
        creds = bench_credentials(self.ledger_mod) if test_key else no_credentials
        self.ledger = self.ledger_mod.VerifiedLocalLedger(db_path, creds)
        self.creds = creds
        self.counts = {"STORED": 0, "DUPLICATE": 0, "REJECTED": 0,
                       "CONFLICT": 0, "UNCERTAIN": 0}

    def handle_line(self, text: str) -> Optional[str]:
        m = LINE.search(text)
        if not m:
            return None
        raw = bytes.fromhex(m["obj"])
        if len(raw) != int(m["len"]):
            self.counts["REJECTED"] += 1
            return f"RED   gw={m['gw']} slot={m['slot']}: uzunluk tutmuyor"
        state = self.ledger.ingest(raw).state
        S = self.ledger_mod.LedgerState
        label = {S.VERIFIED_STORED: "STORED", S.VERIFIED_DUPLICATE: "DUPLICATE",
                 S.REJECTED: "REJECTED", S.CONTENT_CONFLICT: "CONFLICT",
                 S.UNCERTAIN: "UNCERTAIN"}[state]
        self.counts[label] += 1
        word = {"STORED": "YENİ ", "DUPLICATE": "VARDI", "REJECTED": "RED  ",
                "CONFLICT": "ÇAKIŞ", "UNCERTAIN": "BELİRSİZ"}[label]
        if label in ("REJECTED", "UNCERTAIN"):
            return f"{word} gw={m['gw']} page={m['page']} slot={m['slot']}"
        item = self.ledger_mod.open_product_frame(raw, self.creds)
        if item.family != 1:
            return f"{word} tasma={item.device_id.hex().upper()} seq={item.sequence} (EVENT)"
        return f"{word} {human(describe_periodic(item.device_id, item.plaintext))}"

    def summary(self) -> str:
        c = self.counts
        return (f"ÖZET yeni={c['STORED']} vardı={c['DUPLICATE']} red={c['REJECTED']} "
                f"çakışma={c['CONFLICT']} belirsiz={c['UNCERTAIN']} "
                f"toplam_kayıt={self.ledger.record_count()}")


def lines_from_file(path: str) -> Iterable[str]:
    stream = sys.stdin if path == "-" else open(path, encoding="utf-8", errors="replace")
    with stream:
        for line in stream:
            yield line.rstrip("\r\n")


def run_port(edge: BenchEdge, port: str, dump: bool) -> None:
    try:
        import serial  # pyserial
    except ImportError:
        raise SystemExit("pyserial yok: python3 -m pip install pyserial")
    with serial.Serial(port, 115200, timeout=1) as ser:
        time.sleep(0.5)
        if dump:
            ser.write(b"BENCH GW DUMP\n")
        print(f"[{port}] dinleniyor; çıkış Ctrl+C", flush=True)
        buf = b""
        try:
            while True:
                buf += ser.read(512)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    out = edge.handle_line(text)
                    if out:
                        print(out, flush=True)
                    elif text.startswith("EDGE DUMP END"):
                        print(f"[gateway] {text} | {edge.summary()}", flush=True)
        except KeyboardInterrupt:
            pass
    print(edge.summary())


def list_db(db_path: Path, test_key: bool) -> None:
    edge = BenchEdge(db_path, test_key)
    with sqlite3.connect(str(db_path)) as db:
        rows = db.execute(
            "SELECT device_id,family,plaintext FROM verified_observations "
            "ORDER BY device_id,incarnation,sequence").fetchall()
    for device, family, plaintext in rows:
        if family == 1:
            print(human(describe_periodic(bytes(device), bytes(plaintext))))
    print(f"toplam={len(rows)}")
    del edge


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--port", help="Gateway USB serial port (/dev/serial/by-id/...)")
    src.add_argument("--file", help="saved Gateway log, or - for stdin")
    src.add_argument("--list", action="store_true", help="print records already in the ledger")
    ap.add_argument("--db", default=str(Path.home() / "orun-edge-bench.sqlite"),
                    help="ledger SQLite file (default: ~/orun-edge-bench.sqlite)")
    ap.add_argument("--bench-test-key", action="store_true",
                    help="accept the PUBLIC SF5L0 test credential (bench only)")
    ap.add_argument("--no-dump", action="store_true",
                    help="with --port: do not request BENCH GW DUMP at start")
    args = ap.parse_args(argv)
    db = Path(os.path.expanduser(args.db))
    if args.list:
        list_db(db, args.bench_test_key)
        return 0
    if not args.bench_test_key:
        print("UYARI: --bench-test-key yok; hiçbir kayıt açılamaz (hepsi RED olur).",
              file=sys.stderr)
    edge = BenchEdge(db, args.bench_test_key)
    if args.port:
        run_port(edge, args.port, not args.no_dump)
        return 0
    if not args.file:
        ap.error("--port, --file or --list required")
    for text in lines_from_file(args.file):
        out = edge.handle_line(text)
        if out:
            print(out)
    print(edge.summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
