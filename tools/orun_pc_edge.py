#!/usr/bin/env python3
"""ORUN PC Edge: sealed Gateway USB/BLE inbox -> authenticated backend upload.

The PC stores exact protected frames (not plaintext, not K_root) with SQLite
WAL+FULL. Another worker retries uploads if the backend or network is offline.
The backend is the only place that decrypts and authenticates Tracker data.

The default PC Edge is receive-only and NEVER sends EDGE ACCEPT/reclaim.
Optional --bench-release-gateway explicitly enables the EXISTING SF5L0
PUBLIC-TEST-KEY HMAC handoff, but only for a specified bench Gateway and only
after backend STORED/DUPLICATE. NOT authorization for a provisioned Gateway or
a production backend signed receipt. Do not deploy this mode on real keys.

Usage:
  python3 tools/orun_pc_edge.py --mode usb --port /dev/serial/by-id/GATEWAY \\
    --backend-url http://100.100.100.100:8090 --token-file ~/.config/orun/backend.token \\
    --edge-id pc-a
  python3 tools/orun_pc_edge.py --mode ble --edge-id pc-a --backend-url ... --token-file ...
  python3 tools/orun_pc_edge.py --mode upload-only --edge-id pc-a --backend-url ... --token-file ...

Dependencies: Python3 stdlib; pyserial (USB), bleak and BlueZ (BLE).
No connection is made to public Internet. Only loopback or Tailscale IPv4.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import asyncio
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import (
    HTTPRedirectHandler, ProxyHandler, Request, build_opener,
)

from orun_edge_bench import (
    EDGE_SERVICE_UUID, EDGE_TO_GATEWAY_UUID, EDGE_TO_EDGE_UUID,
    EdgeReassembler, edge_frames, ACCEPT_REPLY, ACCEPT_WORDS,
    edge_accept_line, edge_accept_binary,
)

USB_HELD = re.compile(
    r"EDGE (?:HELD|PRIORITY) v1 gw=([0-9A-F]{16}) page=(\d+) slot=(\d+) "
    r"gen=([0-9A-F]{16}) len=(\d+) obj=([0-9A-F]+)(?:\s|$)"
)
EDGE_ID = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
VALID_STATES = {"STORED", "DUPLICATE", "CONFLICT", "REJECTED", "UNCERTAIN"}
SUCCESS_STATES = {"STORED", "DUPLICATE"}
QUARANTINE_STATES = {"CONFLICT", "REJECTED"}
MAX_UPLOAD = 50
SLEEP_S = 10
SCHEMA = """
CREATE TABLE IF NOT EXISTS sealed_records (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 digest BLOB NOT NULL UNIQUE CHECK(length(digest)=32),
 frame BLOB NOT NULL CHECK(length(frame) IN (84,100)),
 gateway TEXT,
 first_seen_utc INTEGER NOT NULL,
 backend_state TEXT,
 last_attempt_utc INTEGER
);
CREATE INDEX IF NOT EXISTS sealed_pending ON sealed_records(backend_state,id);
CREATE TABLE IF NOT EXISTS gateway_holds (
 gateway TEXT NOT NULL CHECK(length(gateway)=16),
 page INTEGER NOT NULL,
 slot INTEGER NOT NULL,
 generation TEXT NOT NULL CHECK(length(generation)=16),
 digest BLOB NOT NULL CHECK(length(digest)=32),
 seen_utc INTEGER NOT NULL,
 accept_state TEXT,
 PRIMARY KEY(gateway,page,slot,generation)
);
CREATE INDEX IF NOT EXISTS gateway_holds_ready
 ON gateway_holds(gateway,accept_state,seen_utc);
"""


def checked_backend(url: str) -> str:
    parsed = urlsplit(url)
    if (parsed.scheme != "http" or parsed.username is not None or
            parsed.password is not None or parsed.query or parsed.fragment or
            parsed.path not in ("", "/") or not parsed.hostname):
        raise ValueError("backend URL must be http://<private-ip>:8090")
    try:
        host = ipaddress.ip_address(parsed.hostname)
        port = parsed.port
    except ValueError as exc:
        raise ValueError("backend must be a literal private IPv4 address") from exc
    if (host.version != 4 or host not in ipaddress.ip_network("100.64.0.0/10")
            and not host.is_loopback or port != 8090):
        # Port fixed to the one in the backend systemd installer.
        raise ValueError("backend requires Tailscale IPv4 (or loopback), port 8090")
    return f"http://{host}:8090"


def read_token(file: Path) -> str:
    metadata = file.stat()
    if not stat.S_ISREG(metadata.st_mode) or stat.S_IMODE(metadata.st_mode) & 0o077:
        raise ValueError("token file must be owner-only (chmod 600)")
    token = file.read_text(encoding="utf-8").strip()
    if len(token) < 8 or "\n" in token or "\r" in token:
        raise ValueError("token file must contain a single token of 8+ chars")
    return token


class Inbox:
    def __init__(self, file: Path):
        file = Path(file).expanduser()
        file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.path = file
        with self.conn() as db:
            db.executescript(SCHEMA)

    @contextmanager
    def conn(self):
        db = sqlite3.connect(str(self.path), timeout=5)
        try:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA synchronous=FULL")
            db.execute("PRAGMA busy_timeout=5000")
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    def ingest(self, raw: bytes, gateway: str | None = None) -> bool:
        # This is a deliberately untrusted *sealed* queue. No AEAD validity
        # is claimed here; only the backend may authenticate and release it.
        if (len(raw) not in (84, 100) or raw[:3] != b"\x02\x04\x02"
                or (gateway is not None and not re.fullmatch("[0-9A-F]{16}", gateway))):
            return False
        now = int(time.time())
        with self.conn() as db:
            db.execute(
                "INSERT OR IGNORE INTO sealed_records "
                "(digest,frame,gateway,first_seen_utc) VALUES (?,?,?,?)",
                (hashlib.sha256(raw).digest(), raw, gateway, now),
            )
        return True

    def observe_hold(self, gateway: str, page: int, slot: int,
                     generation: str, raw: bytes) -> None:
        """Keep Gateway handle tied to exact sealed bytes, not just a slot."""
        if (not re.fullmatch(r"[0-9A-F]{16}", gateway) or
                not re.fullmatch(r"[0-9A-F]{16}", generation) or
                not 0 <= page <= 65535 or not 0 <= slot <= 65535):
            raise ValueError("invalid gateway custody handle")
        digest = hashlib.sha256(raw).digest()
        with self.conn() as db:
            db.execute(
                "INSERT OR IGNORE INTO gateway_holds "
                "(gateway,page,slot,generation,digest,seen_utc) "
                "VALUES (?,?,?,?,?,?)",
                (gateway, page, slot, generation, digest, int(time.time())),
            )

    def ready_hold(self, gateway: str) -> dict | None:
        """Never produce an ACCEPT for pending, rejected, or conflict frames."""
        with self.conn() as db:
            row = db.execute(
                "SELECT h.gateway,h.page,h.slot,h.generation,h.digest,r.frame "
                "FROM gateway_holds h JOIN sealed_records r ON h.digest=r.digest "
                "WHERE h.gateway=? AND h.accept_state IS NULL "
                "AND r.backend_state IN ('STORED','DUPLICATE') "
                "ORDER BY h.seen_utc,h.page,h.slot LIMIT 1", (gateway,)
            ).fetchone()
        if row is None:
            return None
        return {"gateway": row[0], "page": row[1], "slot": row[2],
                "generation": row[3], "digest": row[4], "frame": bytes(row[5])}

    def record_accept(self, h: dict, state: str) -> None:
        if state not in ("OK", "UNKNOWN", "REJECTED", "FAIL", "TIMEOUT"):
            raise ValueError("invalid Gateway accept result")
        with self.conn() as db:
            db.execute(
                "UPDATE gateway_holds SET accept_state=? WHERE "
                "gateway=? AND page=? AND slot=? AND generation=? "
                "AND digest=? AND accept_state IS NULL",
                (state, h["gateway"], h["page"], h["slot"],
                 h["generation"], h["digest"]),
            )

    def batch(self) -> list[dict]:
        with self.conn() as db:
            rows = db.execute(
                "SELECT id,frame,gateway,first_seen_utc FROM sealed_records "
                "WHERE backend_state IS NULL ORDER BY id LIMIT ?", (MAX_UPLOAD - 1,),
            ).fetchall()
            newest = db.execute(
                "SELECT id,frame,gateway,first_seen_utc FROM sealed_records "
                "WHERE backend_state IS NULL ORDER BY id DESC LIMIT 1",
            ).fetchone()
        if newest and newest[0] not in {r[0] for r in rows}:
            rows.insert(0, newest)  # newest-first without starving old backlog
        return [
            {"id": row[0], "frame": bytes(row[1]).hex().upper(),
             "gateway": row[2], "first_seen_utc": row[3]}
            for row in rows
        ]

    def acknowledge(self, rows: list[dict], states: list[str]) -> None:
        if len(rows) != len(states) or any(x not in VALID_STATES for x in states):
            raise ValueError("malformed backend response: no acknowledgements written")
        with self.conn() as db:
            for row, state in zip(rows, states):
                if state in SUCCESS_STATES | QUARANTINE_STATES:
                    db.execute(
                        "UPDATE sealed_records SET backend_state=?,last_attempt_utc=? "
                        "WHERE id=? AND backend_state IS NULL",
                        (state, int(time.time()), row["id"]),
                    )

    def counts(self) -> dict:
        with self.conn() as db:
            result = {"pending": 0, "delivered": 0, "quarantined": 0}
            for state, count in db.execute(
                    "SELECT backend_state,count(*) FROM sealed_records GROUP BY backend_state"):
                key = ("pending" if state is None else
                       "delivered" if state in SUCCESS_STATES else "quarantined")
                result[key] += count
            return result


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Uploader:
    def __init__(self, inbox: Inbox, url: str, token_file: Path, edge_id: str):
        if not EDGE_ID.fullmatch(edge_id):
            raise ValueError("edge id: 1-64 ASCII letters, digits, ._-")
        self.inbox = inbox
        self.base = checked_backend(url)
        self.token_file = token_file
        self.edge_id = edge_id
        # Never obey HTTP_PROXY/HTTPS_PROXY, and never follow redirects with
        # a bearer token. The target must stay on the private tailnet.
        self.opener = build_opener(ProxyHandler({}), NoRedirect())

    def once(self) -> int:
        rows = self.inbox.batch()
        if not rows:
            return 0
        body = json.dumps({
            "edge": self.edge_id,
            "records": [
                {"frame": r["frame"], "gateway": r["gateway"],
                 "first_seen_utc": r["first_seen_utc"]}
                for r in rows
            ],
            "sessions": [],
        }, separators=(",", ":")).encode("utf-8")
        req = Request(
            self.base + "/v1/edge/batch", data=body, method="POST",
            headers={"Authorization": "Bearer " + read_token(self.token_file),
                     "Content-Type": "application/json"},
        )
        with self.opener.open(req, timeout=15) as res:
            if res.status != 200:
                raise RuntimeError("backend HTTP status " + str(res.status))
            reply = json.load(res)
        if not isinstance(reply, dict) or not isinstance(reply.get("results"), list):
            raise ValueError("invalid backend JSON response")
        states = reply["results"]
        self.inbox.acknowledge(rows, states)
        return sum(x in SUCCESS_STATES for x in states)

    def forever(self) -> None:
        while True:
            try:
                sent = self.once()
                if sent:
                    print("[backend] confirmed", sent, self.inbox.counts(), flush=True)
            except (HTTPError, URLError, ValueError, OSError, RuntimeError,
                    TimeoutError, sqlite3.Error) as exc:
                # Do not print request headers or secrets.
                if isinstance(exc, HTTPError):
                    err = "HTTP " + str(exc.code)
                else:
                    err = type(exc).__name__
                print("[backend] upload pending:", err, flush=True)
            time.sleep(SLEEP_S)


def handle_usb_line(line: str, inbox: Inbox) -> bool:
    m = USB_HELD.search(line)
    if not m:
        return False
    hexdata = m[6]
    if len(hexdata) not in (168, 200) or len(hexdata) != 2 * int(m[5]):
        return False
    raw = bytes.fromhex(hexdata)
    stored = inbox.ingest(raw, m[1])
    if stored:
        inbox.observe_hold(m[1], int(m[2]), int(m[3]), m[4], raw)
    return stored


class BenchUsbRelease:
    """Opt-in PUBLIC SF5L0 test-key handoff, after backend verification only.

    No release on a network timeout, UNCERTAIN, CONFLICT or REJECTED response.
    The Gateway must independently authenticate the exact page/slot/gen/MAC.
    """
    def __init__(self, inbox: Inbox, gateway: str):
        if not re.fullmatch(r"[0-9A-F]{16}", gateway):
            raise ValueError("bench release requires exact uppercase Gateway ID")
        self.inbox = inbox
        self.gateway = gateway
        self.pending = None
        self.attempts = 0
        self.last_sent = 0.0

    def acknowledge_gateway(self, word: str, page: int, slot: int) -> None:
        if self.pending is None:
            return
        if (page, slot) != (self.pending["page"], self.pending["slot"]):
            return
        if word == "BUSY":
            self.last_sent = 0.0   # retry same exact handle on next loop
            return
        self.inbox.record_accept(self.pending, word)
        print("[bench accept] gateway result:", word,
              "page=", page, "slot=", slot, flush=True)
        self.pending = None
        self.attempts = 0

    def observe(self, line: str) -> None:
        reply = ACCEPT_REPLY.search(line)
        if not reply or reply["page"] == "?" or reply["slot"] == "?":
            return
        self.acknowledge_gateway(
            reply["word"], int(reply["page"]), int(reply["slot"]))

    def next_eligible(self, now: float) -> dict | None:
        if self.pending is not None:
            if now - self.last_sent < 3.0:
                return None
            if self.attempts >= 3:
                self.inbox.record_accept(self.pending, "TIMEOUT")
                print("[bench accept] TIMEOUT; no further automatic retry",
                      flush=True)
                self.pending = None
                self.attempts = 0
                return None
        else:
            self.pending = self.inbox.ready_hold(self.gateway)
            if self.pending is None:
                return None
            self.attempts = 0
        h = self.pending
        self.last_sent = now
        self.attempts += 1
        print("[bench accept] sent", self.attempts,
              "page=", h["page"], "slot=", h["slot"], flush=True)
        return h

    def poll(self, serial_port, now: float) -> None:
        h = self.next_eligible(now)
        if h is None:
            return
        line = edge_accept_line(
            int(h["gateway"], 16), h["page"], h["slot"],
            int(h["generation"], 16), h["frame"])
        serial_port.write((line + "\n").encode("ascii"))


class BenchBleRelease(BenchUsbRelease):
    """Same acknowledged custody queue over BLE (SF5L6 20-byte GATT frames)."""

    def observe_message(self, msg: bytes) -> None:
        if len(msg) == 6 and msg[0] == 4:
            word = ACCEPT_WORDS.get(msg[5], "REJECTED")
            self.acknowledge_gateway(
                word, int.from_bytes(msg[1:3], "big"),
                int.from_bytes(msg[3:5], "big"))

    async def poll_ble(self, client, now: float, message_id: int) -> int:
        h = self.next_eligible(now)
        if h is None:
            return message_id
        binary = edge_accept_binary(
            int(h["gateway"], 16), h["page"], h["slot"],
            int(h["generation"], 16), h["frame"])
        message_id = (message_id + 1) & 0xFF
        for frame in edge_frames(binary, message_id):
            await client.write_gatt_char(
                EDGE_TO_GATEWAY_UUID, frame, response=True)
        return message_id


def handle_ble_message(msg: bytes, inbox: Inbox, gateway: str | None) -> str | None:
    if len(msg) == 13 and msg[0] == 1:
        return msg[1:9].hex().upper()
    if (gateway and msg and msg[0] in (2, 6) and len(msg) >= 18
            and len(msg) == 18 + msg[17] and msg[17] in (84, 100)):
        raw = bytes(msg[18:])
        if inbox.ingest(raw, gateway):
            inbox.observe_hold(
                gateway, int.from_bytes(msg[1:3], "big"),
                int.from_bytes(msg[3:5], "big"),
                msg[5:13].hex().upper(), raw)
    return gateway


def usb_forever(inbox: Inbox, port: str,
                release_gateway: str | None = None) -> None:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Install python3-serial (pyserial) for USB mode") from exc
    while True:
        try:
            with serial.Serial(port, 115200, timeout=0.5) as ser:
                print("[usb] Gateway connected:", port, flush=True)
                release = (BenchUsbRelease(inbox, release_gateway)
                           if release_gateway else None)
                last_dump = 0.
                while True:
                    now = time.monotonic()
                    if now - last_dump >= 120:
                        # Both commands only read Gateway custody state.
                        # BENCH? also exposes reclaimed-page totals.
                        ser.write(b"BENCH?\n")
                        ser.write(b"BENCH GW DUMP\n")
                        last_dump = now
                    raw = ser.read_until(b"\n", size=4096)
                    if raw:
                        text = raw.decode("ascii", "replace").rstrip("\r\n")
                        if handle_usb_line(text, inbox):
                            print("[usb] sealed record committed",
                                  inbox.counts(), flush=True)
                        if (text.startswith("EDGE DUMP END count=") or
                                text.startswith("EDGE RECLAIM page=") or
                                text.startswith("BENCH GATEWAY id=") or
                                (text.startswith("BENCH GATEWAY ")
                                 and ("reclaimed=" in text or "held=" in text))):
                            print("[gateway]", text, flush=True)
                        if release:
                            release.observe(text)
                    if release:
                        release.poll(ser, time.monotonic())
        except (OSError, serial.SerialException) as exc:
            print("[usb] disconnected; reconnecting:", type(exc).__name__, flush=True)
            time.sleep(5)


async def request_ble_dump(client, message_id: int = 1) -> None:
    """Request a replay after subscribing to indications; read-only custody operation.

    SF5L6 0x81 DUMP resets the Gateway's scan cursor, not custody/ACK state.
    """
    for frame in edge_frames(bytes((0x81,)), message_id):
        await client.write_gatt_char(EDGE_TO_GATEWAY_UUID, frame, response=True)


def ble_hello_expired(started: float, now: float,
                      hello_received: bool) -> bool:
    """A link that never delivers a Gateway HELLO is not a usable Edge session."""
    return not hello_received and now - started >= 20.0


async def discover_ble_gateway(scanner, address: str | None):
    """Resolve a fresh BLEDevice; never pass a bare MAC to BleakClient."""
    if address:
        return await scanner.find_device_by_address(address, timeout=15)

    def match(dev, adv):
        return EDGE_SERVICE_UUID in [
            s.lower() for s in (adv.service_uuids or [])
        ]

    return await scanner.find_device_by_filter(match, timeout=20)


def ble_forever(inbox: Inbox, address: str | None,
                release_gateway: str | None = None) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise SystemExit("Install bleak/BlueZ for BLE mode") from exc

    async def run():
        while True:
            try:
                # Fresh BLEDevice avoids BleakClient's implicit MAC scan,
                # which can race BlueZ device-object removal.
                device = await discover_ble_gateway(BleakScanner, address)
                if device is None:
                    print("[ble] Gateway not discovered; retrying", flush=True)
                    await asyncio.sleep(5)
                    continue
                queue = asyncio.Queue()
                def received(_, data):
                    queue.put_nowait(bytes(data))
                async with BleakClient(device, timeout=20) as client:
                    await client.start_notify(EDGE_TO_EDGE_UUID, received)
                    await request_ble_dump(client)
                    print("[ble] Gateway connected; DUMP requested", flush=True)
                    reassembler = EdgeReassembler()
                    gateway = None
                    release = None
                    mid = 0
                    started_ble = time.monotonic()
                    seen_frames = 0
                    hello_received = False
                    while client.is_connected:
                        try:
                            frame = await asyncio.wait_for(queue.get(), timeout=0.1)
                        except asyncio.TimeoutError:
                            frame = None  # poll backend acknowledgements while idle
                        if frame:
                            seen_frames += 1
                            if seen_frames == 1:
                                print("[ble] first GATT frame received",
                                      "bytes=", len(frame), flush=True)
                            msg = reassembler.push(frame)
                            if msg:
                                gateway = handle_ble_message(msg, inbox, gateway)
                                if msg[0] == 1 and len(msg) == 13:
                                    hello_received = True
                                    held = int.from_bytes(msg[9:11], "big")
                                    capacity = int.from_bytes(msg[11:13], "big")
                                    print("[ble] HELLO", gateway,
                                          "held=", held, "/", capacity, flush=True)
                                elif msg[0] in (2, 6) and gateway:
                                    print("[ble] HELD/PREVIEW stored",
                                          inbox.counts(), flush=True)
                                elif msg[0] == 3 and len(msg) == 3:
                                    print("[ble] DUMP END count=",
                                          int.from_bytes(msg[1:3], "big"),
                                          flush=True)
                                elif msg[0] == 5 and len(msg) == 4:
                                    print("[ble] RECLAIM page=",
                                          int.from_bytes(msg[1:3], "big"),
                                          "ok=", bool(msg[3]), flush=True)
                                if release_gateway and gateway == release_gateway:
                                    if release is None:
                                        release = BenchBleRelease(inbox, gateway)
                                    release.observe_message(msg)
                                elif release_gateway and gateway != release_gateway:
                                    print("[ble] Gateway ID mismatch; ACCEPT disabled",
                                          flush=True)
                                    release = None
                        if ble_hello_expired(
                                started_ble, time.monotonic(), hello_received):
                            raise TimeoutError(
                                "Gateway HELLO missing after 20s; "
                                f"received GATT frames={seen_frames}")
                        if release:
                            mid = await release.poll_ble(client, time.monotonic(), mid)
                    print("[ble] disconnected",
                          "HELLO=", hello_received,
                          "GATT frames=", seen_frames, flush=True)
            except Exception as exc:
                print("[ble] reconnecting after", type(exc).__name__,
                      str(exc), flush=True)
                await asyncio.sleep(5)

    asyncio.run(run())


def main() -> None:
    os.umask(0o077)
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mode", choices=("usb", "ble", "upload-only"), required=True)
    ap.add_argument("--port", help="Gateway USB serial device, preferably /dev/serial/by-id")
    ap.add_argument("--ble-address", help="optional fixed BLE device address")
    ap.add_argument("--db", type=Path,
                    default=Path.home() / ".local/share/orun/pc-edge-inbox.sqlite")
    ap.add_argument("--backend-url", required=True)
    ap.add_argument("--token-file", type=Path, required=True)
    ap.add_argument("--edge-id", required=True)
    ap.add_argument("--bench-release-gateway",
                    help="EXPLICIT SF5L0 PUBLIC TEST KEY release on USB/BLE; "
                         "expected 16-hex Gateway ID (NEVER production)")
    args = ap.parse_args()
    if args.mode == "usb" and not args.port:
        ap.error("--port is required for USB mode")
    if args.bench_release_gateway and args.mode not in ("usb", "ble"):
        ap.error("--bench-release-gateway requires USB or BLE")
    if args.bench_release_gateway and not re.fullmatch(
            r"[0-9A-F]{16}", args.bench_release_gateway):
        ap.error("--bench-release-gateway needs exact uppercase 16-hex ID")
    # Fail before touching local SQLite when destination/token are wrong.
    checked_backend(args.backend_url)
    read_token(args.token_file.expanduser())
    inbox = Inbox(args.db)
    uploader = Uploader(inbox, args.backend_url, args.token_file.expanduser(), args.edge_id)
    print("[pc-edge] spool:", inbox.path, inbox.counts(), flush=True)
    if args.bench_release_gateway:
        print("[pc-edge] WARNING: PUBLIC TEST-KEY bench release for",
              args.bench_release_gateway, "NOT real-device authorization",
              flush=True)
    else:
        print("[pc-edge] no EDGE ACCEPT/reclaim: Gateway retains custody",
              flush=True)
    if args.mode == "upload-only":
        uploader.forever()
    else:
        threading.Thread(target=uploader.forever, daemon=True).start()
        if args.mode == "usb":
            usb_forever(inbox, args.port, args.bench_release_gateway)
        else:
            ble_forever(inbox, args.ble_address, args.bench_release_gateway)


if __name__ == "__main__":
    main()
