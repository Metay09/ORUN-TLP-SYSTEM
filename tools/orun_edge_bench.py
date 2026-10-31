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

SF5L3: --serve PORT also serves a map page (tools/edge_map) and a read-only
JSON view of the ledger at /api/points. Default bind is 127.0.0.1; use --host
to expose it (e.g. the Tailscale address). Leaflet is vendored; only the map
tiles come from the internet.

Examples:
  python3 tools/orun_edge_bench.py --bench-test-key --port /dev/ttyACM0
  python3 tools/orun_edge_bench.py --bench-test-key --port /dev/ttyACM0 --serve 8080
  python3 tools/orun_edge_bench.py --serve 8080          # map of the ledger only
  python3 tools/orun_edge_bench.py --bench-test-key --file gateway.log
  python3 tools/orun_edge_bench.py --list
"""
from __future__ import annotations

import argparse
import bisect
import collections
import hashlib
import hmac as hmac_mod
import datetime as dt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
import threading
import time
from typing import Iterable, Optional

ROOT = Path(__file__).resolve().parents[1]
LEDGER_PATH = ROOT / "backend" / "candidates" / "product_secure_verified_ledger.py"
MAP_DIR = ROOT / "tools" / "edge_map"
_LEAFLET = "/vendor/leaflet-1.9.4/"
# Only these files are ever served (no directory walking).
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    _LEAFLET + "leaflet.js": ("vendor/leaflet-1.9.4/leaflet.js", "application/javascript"),
    _LEAFLET + "leaflet.css": ("vendor/leaflet-1.9.4/leaflet.css", "text/css"),
    _LEAFLET + "images/layers.png": ("vendor/leaflet-1.9.4/images/layers.png", "image/png"),
    _LEAFLET + "images/layers-2x.png": ("vendor/leaflet-1.9.4/images/layers-2x.png", "image/png"),
}

# Same public vector as firmware/src/sf5l0_bench.cpp (and the SF3 qualification).
TEST_CREDENTIAL_ID = bytes(range(0xA0, 0xB0))
TEST_ROOT = bytes(range(0x00, 0x20))
TEST_KEY_EPOCH = 0x01020304

LINE = re.compile(
    r"EDGE HELD v1 gw=(?P<gw>[0-9A-F]{16}) page=(?P<page>\d+) slot=(?P<slot>\d+) "
    r"gen=(?P<gen>[0-9A-F]{16}) len=(?P<len>\d+) obj=(?P<obj>[0-9A-F]+)")

# SF5L4: the Gateway's own radio line for the frame it is about to take into
# custody. Its RSSI/SNR is how strongly the GATEWAY heard the collar.
RX_LINE = re.compile(
    r"RX PRODUCT_SECURE queued family=\d+ length=\d+ rssi=(?P<rssi>-?\d+) "
    r"snr=(?P<snr>-?\d+)")
CUSTODY_STARTED = "BENCH GW RX record; durable custody write started"
RX_NOT_NEW = ("BENCH GW RX duplicate already held", "BENCH GW RX denied")

RECEIPTS_SQL = (
    "CREATE TABLE IF NOT EXISTS edge_receipts ("
    " device_id BLOB NOT NULL, incarnation BLOB NOT NULL, sequence INTEGER NOT NULL,"
    " first_seen_utc INTEGER NOT NULL, gw_rssi INTEGER, gw_snr INTEGER,"
    " PRIMARY KEY (device_id,incarnation,sequence))")

# SF5L5 bench Edge accept: after the ledger has durably committed a held
# record, tell the Gateway it may hand it off (and later erase its page).
# Same construction as Sf5l0EdgeHandoff::mac (firmware/src/sf5l0_bench.cpp);
# PUBLIC test key, so this proves the mechanism, not the Edge's identity.
ACCEPT_REPLY = re.compile(
    r"EDGE ACCEPT (?P<word>OK|UNKNOWN|BUSY|REJECTED|FAIL)(?: reason=\w+)? "
    r"page=(?P<page>\d+|\?) slot=(?P<slot>\d+|\?)")
RECLAIM = re.compile(r"EDGE RECLAIM page=(?P<page>\d+) (?P<what>started|OK|FAIL)")
ACCEPT_TIMEOUT_S = 3.0


def edge_accept_mac(gateway: int, page: int, slot: int, generation: int,
                    obj: bytes) -> bytes:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    info = (b"ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1" +
            gateway.to_bytes(8, "big")).ljust(57, b"\0")
    key = HKDF(hashes.SHA256(), 32, TEST_CREDENTIAL_ID, info).derive(TEST_ROOT)
    msg = (b"ORUN-EDGE-ACCEPT-v1" + gateway.to_bytes(8, "big") +
           page.to_bytes(2, "big") + slot.to_bytes(2, "big") +
           generation.to_bytes(8, "big") +
           hashlib.sha256(obj).digest()).ljust(102, b"\0")
    return hmac_mod.new(key, msg, hashlib.sha256).digest()


def edge_accept_binary(gateway: int, page: int, slot: int, generation: int,
                       obj: bytes) -> bytes:
    """edge_link.h kAccept: type(1) page(2) slot(2) gen(8) mac(32)."""
    mac = edge_accept_mac(gateway, page, slot, generation, obj)
    return (bytes([0x82]) + page.to_bytes(2, "big") + slot.to_bytes(2, "big") +
            generation.to_bytes(8, "big") + mac)


# ---- SF5L6 BLE Edge link (firmware/include/edge_link.h) --------------------
EDGE_SERVICE_UUID = "6f9a1c20-5b3e-4d8a-9e61-2c4b7a0d3e51"
EDGE_TO_GATEWAY_UUID = "6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51"
EDGE_TO_EDGE_UUID = "6f9a1c22-5b3e-4d8a-9e61-2c4b7a0d3e51"
EDGE_FRAME = 20
EDGE_FRAME_DATA = 16
EDGE_MAX_MESSAGE = 160
ACCEPT_WORDS = {0: "OK", 1: "OK", 2: "UNKNOWN", 3: "BUSY", 4: "REJECTED",
                5: "REJECTED", 6: "REJECTED", 7: "FAIL"}


def edge_frames(message: bytes, message_id: int) -> list:
    """Split one logical message into 20-byte frames (edge_link Fragmenter)."""
    if not 0 < len(message) <= EDGE_MAX_MESSAGE:
        raise ValueError("message size")
    frames, offset, index = [], 0, 0
    while offset < len(message):
        chunk = message[offset:offset + EDGE_FRAME_DATA]
        flags = (1 if index == 0 else 0) | (2 if offset + len(chunk) == len(message) else 0)
        frames.append(bytes([1, flags, message_id & 0xFF, index]) + chunk)
        offset += len(chunk)
        index += 1
    return frames


class EdgeReassembler:
    """Mirror of edge_link::Reassembler; returns a message when complete."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.active, self.buf, self.mid, self.next_index = False, b"", 0, 0

    def push(self, frame: bytes) -> Optional[bytes]:
        if (len(frame) <= 4 or len(frame) > EDGE_FRAME or frame[0] != 1 or
                frame[1] & ~3):
            self.reset()
            return None
        start, end = bool(frame[1] & 1), bool(frame[1] & 2)
        if start:
            if frame[3] != 0:
                self.reset()
                return None
            self.active, self.buf, self.mid, self.next_index = True, b"", frame[2], 0
        elif not self.active or frame[2] != self.mid or frame[3] != self.next_index:
            self.reset()
            return None
        self.buf += frame[4:]
        self.next_index += 1
        if len(self.buf) > EDGE_MAX_MESSAGE:
            self.reset()
            return None
        if not end:
            return None
        done, self.active = self.buf, False
        return done


def edge_accept_line(gateway: int, page: int, slot: int, generation: int,
                     obj: bytes) -> str:
    mac = edge_accept_mac(gateway, page, slot, generation, obj)
    return (f"EDGE ACCEPT v1 page={page} slot={slot} gen={generation:016X} "
            f"mac={mac.hex().upper()}")


# Periods during which this tool was attached to a live Gateway. Only inside
# such a period can "nothing arrived" mean "no radio link" (and not "nobody
# was listening").
SESSIONS_SQL = (
    "CREATE TABLE IF NOT EXISTS edge_sessions ("
    " started_utc INTEGER NOT NULL, alive_utc INTEGER NOT NULL)")

# Link classification of one record, for the coverage view (seconds).
LIVE_S = 30    # reached the PC this soon after capture: sent from (about) there
CUT_S = 180    # later than this: queued somewhere, or recorded with no link
NEAR_S = 150   # a live arrival this close in capture time proves the link was up

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
        "time": None, "lat": None, "lon": None, "alt_m": None, "sats": None,
        "hdop": None, "fix_age_s": None, "battery_mv": None, "battery": None,
        "active_s": None, "covered_s": None, "source": SOURCES.get(p[2], "?"),
    }
    if flags & 1:
        out["time"] = dt.datetime.fromtimestamp(
            _u(p[16:20]), tz=dt.timezone.utc).isoformat().replace("+00:00", "Z")
    if flags & 2:
        out["lat"] = _i(p[24:28]) / 1e7
        out["lon"] = _i(p[28:32]) / 1e7
        if flags & 4:   # GNSS height above the WGS84 ellipsoid, metres
            out["alt_m"] = _i(p[32:36]) / 1000
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
    def __init__(self, db_path: Path, test_key: bool, clock=time.time,
                 accept: bool = True, registry: Optional[Path] = None):
        self.ledger_mod = load_ledger_module()
        bench = bench_credentials(self.ledger_mod) if test_key else no_credentials
        real = no_credentials
        if registry is not None and Path(registry).exists():
            # Step 3A: real per-Tracker credentials from the backend registry.
            sys.path.insert(0, str(ROOT / "backend"))
            import orun_credentials as oc
            real = oc.CredentialRegistry(registry).lookup(self.ledger_mod)

        def creds(device: bytes, epoch: int):
            return real(device, epoch) or bench(device, epoch)
        self.ledger = self.ledger_mod.VerifiedLocalLedger(db_path, creds)
        self.creds = creds
        self.db_path = Path(db_path)
        self.clock = clock
        # Radio facts of the frame now being taken into custody: RX line ->
        # "custody write started" -> EDGE HELD. Anything else drops them, so
        # a DUMP line never inherits a stranger's signal strength.
        self._rx = None
        self._armed = None
        self._session = None
        self.counts = {"STORED": 0, "DUPLICATE": 0, "REJECTED": 0,
                       "CONFLICT": 0, "UNCERTAIN": 0}
        # Edge accepts are only meaningful for records this ledger verified.
        self.accept = accept and test_key
        self._accepts = collections.deque()   # (page, slot, gen) -> line
        self._queued = set()
        self._in_flight = None                # (key, line, sent_at)
        self.handed_off = 0
        self.accept_rejected = 0
        self.gateway_id = None   # from the BLE HELLO
        # Mirrors Android EdgeCore: a DUMP END count is trustworthy only
        # when every distinct, complete HELD announcement was seen.
        self._dump_seen = set()
        with sqlite3.connect(str(self.db_path)) as db:
            db.execute(RECEIPTS_SQL)
            db.execute(SESSIONS_SQL)

    def session_alive(self) -> None:
        """Record that a Gateway is attached and being read right now."""
        now = int(self.clock())
        with sqlite3.connect(str(self.db_path)) as db:
            if self._session is None:
                self._session = db.execute(
                    "INSERT INTO edge_sessions VALUES (?,?)", (now, now)).lastrowid
            else:
                db.execute("UPDATE edge_sessions SET alive_utc=? WHERE rowid=?",
                           (now, self._session))

    def _receipt(self, item, link) -> None:
        rssi, snr = link if link else (None, None)
        with sqlite3.connect(str(self.db_path)) as db:
            # First sighting wins: a later DUMP is not a new arrival.
            db.execute(
                "INSERT OR IGNORE INTO edge_receipts VALUES (?,?,?,?,?,?)",
                (item.device_id, bytes(item.plaintext[4:12]), item.sequence,
                 int(self.clock()), rssi, snr))
            if link:
                db.execute(
                    "UPDATE edge_receipts SET gw_rssi=?, gw_snr=? WHERE "
                    "device_id=? AND incarnation=? AND sequence=? AND gw_rssi IS NULL",
                    (rssi, snr, item.device_id, bytes(item.plaintext[4:12]),
                     item.sequence))

    def _queue_accept(self, gateway: int, page: int, slot: int, gen: int,
                      raw: bytes) -> None:
        key = (page, slot, gen)
        if not self.accept or key in self._queued:
            return
        payload = (edge_accept_line(gateway, page, slot, gen, raw),
                   edge_accept_binary(gateway, page, slot, gen, raw))
        self._queued.add(key)
        self._accepts.append((key, payload))

    def _next(self, now: float):
        if self._in_flight is not None:
            key, payload, sent = self._in_flight
            if now - sent < ACCEPT_TIMEOUT_S:
                return None
            self._accepts.appendleft((key, payload))   # no reply: send again
            self._in_flight = None
        if not self._accepts:
            return None
        key, payload = self._accepts.popleft()
        self._in_flight = (key, payload, now)
        return payload

    def next_accept(self, now: float) -> Optional[str]:
        """The next EDGE ACCEPT line to write (USB), one in flight at a time."""
        payload = self._next(now)
        return payload[0] if payload else None

    def next_accept_binary(self, now: float) -> Optional[bytes]:
        """The same accept as an edge_link message (BLE)."""
        payload = self._next(now)
        return payload[1] if payload else None

    def pending_accepts(self) -> int:
        return len(self._accepts) + (1 if self._in_flight else 0)

    def _accept_reply(self, word: str, page, slot) -> Optional[str]:
        if self._in_flight is None:
            return None
        key, payload, _ = self._in_flight
        if page is None or word == "BUSY":
            self._accepts.appendleft((key, payload))   # store busy: retry
            self._in_flight = None
            return None
        if (page, slot) != (key[0], key[1]):
            return None
        self._in_flight = None
        self._queued.discard(key)
        r = {"word": word}
        if r["word"] in ("OK", "UNKNOWN"):
            # UNKNOWN: already reclaimed (or never held) -- nothing to keep.
            self.handed_off += 1 if r["word"] == "OK" else 0
            return None
        self.accept_rejected += 1
        return (f"UYARI gateway teslimi reddetti page={key[0]} slot={key[1]} "
                f"({r['word']}); kayıt gateway'de kalır")

    def _dump_end(self, count: Optional[int]) -> bool:
        complete = count is not None and count >= 0 and len(self._dump_seen) == count
        self._dump_seen.clear()
        return complete

    def handle_line(self, text: str) -> Optional[str]:
        if text.startswith("EDGE DUMP END"):
            match = re.search(r"\bcount=(\d+)\b", text)
            complete = self._dump_end(int(match.group(1)) if match else None)
            return (f"[gateway] {text} | {self.summary()}"
                    + ("" if complete else
                       " | UYARI: eksik/bozuk aktarım; sayaç kesin değil, yeniden DUMP gerekli"))
        reply = ACCEPT_REPLY.search(text)
        if reply:
            known = reply["page"] != "?"
            return self._accept_reply(
                reply["word"], int(reply["page"]) if known else None,
                int(reply["slot"]) if known else None)
        reclaim = RECLAIM.search(text)
        if reclaim:
            if reclaim["what"] == "OK":
                return f"[gateway] sayfa {reclaim['page']} boşaltıldı (kayıtları teslim edilmişti)"
            if reclaim["what"] == "FAIL":
                return f"UYARI gateway sayfa {reclaim['page']} silinemedi"
            return None
        rx = RX_LINE.search(text)
        if rx:
            self._rx = (int(rx["rssi"]), int(rx["snr"]))
            self._armed = None
            return None
        if CUSTODY_STARTED in text:
            self._armed, self._rx = self._rx, None
            return None
        if any(t in text for t in RX_NOT_NEW):
            self._rx = self._armed = None
            return None
        m = LINE.search(text)
        if not m:
            return None
        link, self._armed, self._rx = self._armed, None, None
        hex_text = m["obj"]
        if len(hex_text) % 2 or len(hex_text) != 2 * int(m["len"]):
            # Truncated or garbled serial line: never guess, never crash.
            self.counts["REJECTED"] += 1
            return (f"RED   gw={m['gw']} page={m['page']} slot={m['slot']}: "
                    f"satır eksik/bozuk ({len(hex_text)} hex, beklenen "
                    f"{2 * int(m['len'])}); BENCH GW DUMP ile tekrar istenebilir")
        return self._ingest_held(int(m["gw"], 16), int(m["page"]), int(m["slot"]),
                                 int(m["gen"], 16), bytes.fromhex(hex_text), link)

    def _ingest_held(self, gateway: int, page: int, slot: int, gen: int,
                     raw: bytes, link) -> str:
        """One held record from the Gateway (USB line or BLE message)."""
        self._dump_seen.add((page, slot))
        m = {"gw": f"{gateway:016X}", "page": page, "slot": slot}
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
        self._receipt(item, link)
        if label in ("STORED", "DUPLICATE"):
            # The ledger commit (synchronous=FULL) has returned: durable here.
            self._queue_accept(gateway, page, slot, gen, raw)
        sig = f" | sinyal {link[0]} dBm SNR {link[1]}" if link else ""
        if item.family != 1:
            return f"{word} tasma={item.device_id.hex().upper()} seq={item.sequence} (EVENT){sig}"
        first, _, rest = human(describe_periodic(item.device_id, item.plaintext)).partition("\n")
        return f"{word} {first}{sig}" + (f"\n{rest}" if rest else "")

    def handle_ble(self, msg: bytes) -> Optional[str]:
        """One complete edge_link message from the Gateway (BLE)."""
        if not msg:
            return None
        t = msg[0]
        if t == 0x01 and len(msg) == 13:
            self.gateway_id = int.from_bytes(msg[1:9], "big")
            self._dump_seen.clear()
            held = int.from_bytes(msg[9:11], "big")
            cap = int.from_bytes(msg[11:13], "big")
            return f"[gateway BLE] {self.gateway_id:016X} bağlı; tuttuğu kayıt {held}/{cap}"
        if t == 0x02 and len(msg) >= 18 and len(msg) == 18 + msg[17]:
            if self.gateway_id is None:
                return None   # HELLO always comes first; never guess the id
            page = int.from_bytes(msg[1:3], "big")
            slot = int.from_bytes(msg[3:5], "big")
            gen = int.from_bytes(msg[5:13], "big")
            link = None
            if msg[13] & 1:
                link = (int.from_bytes(msg[14:16], "big", signed=True),
                        int.from_bytes(msg[16:17], "big", signed=True))
            return self._ingest_held(self.gateway_id, page, slot, gen,
                                     bytes(msg[18:]), link)
        if t == 0x03 and len(msg) == 3:
            count = int.from_bytes(msg[1:3], "big")
            complete = self._dump_end(count)
            return (f"[gateway BLE] DUMP END count={count} | {self.summary()}"
                    + ("" if complete else
                       " | UYARI: eksik aktarım; sayaç kesin değil, yeniden DUMP gerekli"))
        if t == 0x04 and len(msg) == 6:
            return self._accept_reply(ACCEPT_WORDS.get(msg[5], "REJECTED"),
                                      int.from_bytes(msg[1:3], "big"),
                                      int.from_bytes(msg[3:5], "big"))
        if t == 0x05 and len(msg) == 4:
            page = int.from_bytes(msg[1:3], "big")
            return (f"[gateway] sayfa {page} boşaltıldı (kayıtları teslim edilmişti)"
                    if msg[3] else f"UYARI gateway sayfa {page} silinemedi")
        return None

    def summary(self) -> str:
        c = self.counts
        return (f"ÖZET yeni={c['STORED']} vardı={c['DUPLICATE']} red={c['REJECTED']} "
                f"çakışma={c['CONFLICT']} belirsiz={c['UNCERTAIN']} "
                f"toplam_kayıt={self.ledger.record_count()}"
                + (f" teslim={self.handed_off} teslim_red={self.accept_rejected} "
                   f"bekleyen_teslim={self.pending_accepts()}" if self.accept else ""))


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
    # Short read timeout: EDGE ACCEPT replies come within milliseconds and the
    # next accept is sent as soon as the previous one is answered.
    with serial.Serial(port, 115200, timeout=0.1) as ser:
        time.sleep(0.5)
        if dump:
            edge._dump_seen.clear()
            ser.write(b"BENCH GW DUMP\n")
        print(f"[{port}] dinleniyor; çıkış Ctrl+C", flush=True)
        buf = b""
        edge.session_alive()
        beat = time.monotonic()
        try:
            while True:
                buf += ser.read(512)
                if time.monotonic() - beat >= 15:
                    edge.session_alive()
                    beat = time.monotonic()
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    out = edge.handle_line(text)
                    if out:
                        print(out, flush=True)
                line = edge.next_accept(time.monotonic())
                if line:
                    ser.write((line + "\n").encode("ascii"))
        except KeyboardInterrupt:
            pass
        edge.session_alive()
    print(edge.summary())


def run_ble(edge: BenchEdge, address: Optional[str]) -> None:
    """SF5L6: the same Edge work as run_port, over the Gateway's BLE Edge
    service. The Gateway starts a full pass (HELLO, every held record,
    DUMP END) on each connection, then streams new records."""
    try:
        import asyncio
        from bleak import BleakClient, BleakScanner
    except ImportError:
        raise SystemExit("bleak yok: python3 -m pip install bleak")

    async def find():
        if address:
            return address
        print("[BLE] ORUN gateway aranıyor…", flush=True)

        def match(device, adv):
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            return EDGE_SERVICE_UUID in uuids
        return await BleakScanner.find_device_by_filter(match, timeout=20.0)

    async def session(target):
        inbox = asyncio.Queue()
        rx = EdgeReassembler()
        mid = 0

        def on_frame(_char, data):
            inbox.put_nowait(bytes(data))

        async with BleakClient(target, timeout=20.0) as client:
            name = getattr(target, "name", None) or getattr(target, "address", target)
            print(f"[BLE] bağlandı: {name}", flush=True)
            await client.start_notify(EDGE_TO_EDGE_UUID, on_frame)
            edge.session_alive()
            beat = time.monotonic()
            while client.is_connected:
                try:
                    frames = [await asyncio.wait_for(inbox.get(), 0.1)]
                    while not inbox.empty():
                        frames.append(inbox.get_nowait())
                except asyncio.TimeoutError:
                    frames = []
                for frame in frames:
                    msg = rx.push(frame)
                    if msg:
                        out = edge.handle_ble(msg)
                        if out:
                            print(out, flush=True)
                accept = edge.next_accept_binary(time.monotonic())
                if accept:
                    mid = (mid + 1) & 0xFF
                    for frame in edge_frames(accept, mid):
                        await client.write_gatt_char(EDGE_TO_GATEWAY_UUID, frame,
                                                     response=True)
                if time.monotonic() - beat >= 15:
                    edge.session_alive()
                    beat = time.monotonic()
        print("[BLE] bağlantı koptu; yeniden denenecek", flush=True)

    async def forever():
        while True:
            target = await find()
            if target is None:
                print("[BLE] gateway bulunamadı; 5 sn sonra tekrar", flush=True)
                await asyncio.sleep(5)
                continue
            try:
                await session(target)
            except Exception as exc:   # BlueZ/bleak errors: reconnect
                print(f"[BLE] hata: {exc}; 5 sn sonra tekrar", flush=True)
            await asyncio.sleep(5)

    try:
        asyncio.run(forever())
    except KeyboardInterrupt:
        pass
    edge.session_alive()
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


def points_from_db(db_path: Path) -> dict:
    """Read-only map view of the verified ledger: PERIODIC records with a
    valid location, decoded. Opens SQLite read-only on every call."""
    points, skipped = [], 0
    if not Path(db_path).exists():
        return {"count": 0, "skipped": 0, "points": []}
    uri = "file:" + str(Path(db_path).resolve()) + "?mode=ro"
    try:
        with sqlite3.connect(uri, uri=True, timeout=1.0) as db:
            has_receipts = db.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                "name='edge_receipts'").fetchone() is not None
            if has_receipts:
                rows = db.execute(
                    "SELECT o.device_id,o.family,o.plaintext,r.first_seen_utc,"
                    "r.gw_rssi,r.gw_snr FROM verified_observations o "
                    "LEFT JOIN edge_receipts r USING (device_id,incarnation,sequence) "
                    "ORDER BY o.device_id,o.incarnation,o.sequence").fetchall()
            else:
                rows = [r + (None, None, None) for r in db.execute(
                    "SELECT device_id,family,plaintext FROM verified_observations "
                    "ORDER BY device_id,incarnation,sequence").fetchall()]
            sessions = []
            if db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND "
                          "name='edge_sessions'").fetchone():
                sessions = db.execute(
                    "SELECT started_utc,alive_utc FROM edge_sessions").fetchall()
    except sqlite3.OperationalError:
        return {"count": 0, "skipped": 0, "points": []}
    for device, family, plaintext, seen, rssi, snr in rows:
        if family != 1:
            continue
        d = describe_periodic(bytes(device), bytes(plaintext))
        if d["lat"] is None:
            skipped += 1
            continue
        points.append({k: d[k] for k in (
            "tracker", "sequence", "time", "lat", "lon", "alt_m", "sats", "hdop",
            "fix_age_s", "battery_mv", "battery", "active_s", "covered_s",
            "source")})
        pt = points[-1]
        pt["seq"] = pt.pop("sequence")
        pt["period_s"] = d["period_s"]
        pt["rssi"], pt["snr"] = rssi, snr
        pt["received"] = (dt.datetime.fromtimestamp(seen, tz=dt.timezone.utc)
                          .isoformat().replace("+00:00", "Z") if seen else None)
        pt["delay_s"] = None
        if seen and d["time"]:
            recorded = dt.datetime.fromisoformat(d["time"].replace("Z", "+00:00"))
            pt["delay_s"] = max(0, int(seen - recorded.timestamp()))
    classify_links(points, sessions)
    return {"count": len(points), "skipped": skipped,
            "now": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
            "points": points}


def _epoch(iso: Optional[str]) -> Optional[float]:
    if not iso:
        return None
    return dt.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()


def classify_links(points: list, sessions: list) -> None:
    """Set p["link"] for the coverage view; None means "says nothing".

    live:    gateway RSSI known and at the PC within LIVE_S of capture: the
             radio facts belong to the place the record was taken.
    through: captured while this tool was attached to the Gateway and at the
             PC within CUT_S: the link worked there, no usable signal.
    cut:     later than CUT_S, captured while this tool was attached to the
             Gateway, and no record of the same tracker captured within NEAR_S
             arrived live: there was no radio link at that place.
    A late record captured while other records were arriving live only waited
    in the collar's queue; one captured while nothing was listening is
    unknown. Neither is evidence about coverage.
    """
    live_times: dict = {}
    for p in points:
        t = _epoch(p["time"])
        if (t is not None and p["rssi"] is not None and p["delay_s"] is not None
                and p["delay_s"] <= LIVE_S):
            live_times.setdefault(p["tracker"], []).append(t)
    for times in live_times.values():
        times.sort()
    for p in points:
        p["link"] = None
        t = _epoch(p["time"])
        if t is None or p["received"] is None or p["delay_s"] is None:
            continue
        if p["rssi"] is not None and p["delay_s"] <= LIVE_S:
            p["link"] = "live"      # RSSI pairing itself needs a listening tool
        elif not any(a <= t <= b for a, b in sessions):
            continue                # nobody listened when it was captured
        elif p["delay_s"] <= CUT_S:
            p["link"] = "through"
        else:
            times = live_times.get(p["tracker"], [])
            i = bisect.bisect_left(times, t - NEAR_S)
            if not (i < len(times) and times[i] <= t + NEAR_S):
                p["link"] = "cut"


def make_map_server(db_path: Path, host: str, port: int) -> ThreadingHTTPServer:
    class Handler(BaseHTTPRequestHandler):
        server_version = "ORUN-edge-bench"

        def log_message(self, *_args):  # keep the serial output readable
            pass

        def _send(self, code: int, body: bytes, ctype: str, cache: bool) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=3600" if cache else "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802
            path = self.path.split("?", 1)[0]
            if path == "/api/points":
                body = json.dumps(points_from_db(db_path)).encode()
                return self._send(200, body, "application/json", False)
            entry = STATIC_FILES.get(path)
            if entry is None:
                return self._send(404, b"not found", "text/plain", False)
            name, ctype = entry
            body = (MAP_DIR / name).read_bytes()
            return self._send(200, body, ctype, name != "index.html")

    return ThreadingHTTPServer((host, port), Handler)


def start_map_server(db_path: Path, host: str, port: int) -> ThreadingHTTPServer:
    server = make_map_server(db_path, host, port)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    shown = "localhost" if host in ("127.0.0.1", "0.0.0.0") else host
    print(f"[harita] http://{shown}:{server.server_address[1]}/  (çıkış Ctrl+C)", flush=True)
    return server


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--port", help="Gateway USB serial port (/dev/serial/by-id/...)")
    src.add_argument("--file", help="saved Gateway log, or - for stdin")
    src.add_argument("--ble", nargs="?", const="", metavar="ADDRESS",
                     help="Gateway over Bluetooth LE (needs bleak); without "
                          "ADDRESS the first gateway advertising the ORUN Edge "
                          "service is used")
    src.add_argument("--list", action="store_true", help="print records already in the ledger")
    ap.add_argument("--db", default=str(Path.home() / "orun-edge-bench.sqlite"),
                    help="ledger SQLite file (default: ~/orun-edge-bench.sqlite)")
    ap.add_argument("--bench-test-key", action="store_true",
                    help="accept the PUBLIC SF5L0 test credential (bench only)")
    ap.add_argument("--registry", default=str(Path.home() / "orun-credentials.sqlite"),
                    help="step 3A credential registry (real Tracker keys), if present")
    ap.add_argument("--no-dump", action="store_true",
                    help="with --port: do not request BENCH GW DUMP at start")
    ap.add_argument("--no-accept", action="store_true",
                    help="with --port: never tell the Gateway it may hand off "
                         "(records then stay on the Gateway until erased)")
    ap.add_argument("--serve", type=int, metavar="HTTP_PORT",
                    help="also serve the map page and /api/points on this port")
    ap.add_argument("--host", default="127.0.0.1",
                    help="address for --serve (default 127.0.0.1; e.g. your Tailscale IP)")
    args = ap.parse_args(argv)
    db = Path(os.path.expanduser(args.db))
    server = start_map_server(db, args.host, args.serve) if args.serve else None
    if server and not (args.port or args.file or args.list or args.ble is not None):
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            server.shutdown()
            return 0
    if args.list:
        list_db(db, args.bench_test_key)
        return 0
    if not args.bench_test_key:
        print("UYARI: --bench-test-key yok; hiçbir kayıt açılamaz (hepsi RED olur).",
              file=sys.stderr)
    edge = BenchEdge(db, args.bench_test_key,
                     accept=(bool(args.port) or args.ble is not None)
                     and not args.no_accept,
                     registry=Path(args.registry))
    if args.ble is not None:
        run_ble(edge, args.ble or None)
        return 0
    if args.port:
        run_port(edge, args.port, not args.no_dump)
        return 0
    if not args.file:
        ap.error("--port, --ble, --file or --list required")
    for text in lines_from_file(args.file):
        out = edge.handle_line(text)
        if out:
            print(out)
    print(edge.summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
