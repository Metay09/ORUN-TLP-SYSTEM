#!/usr/bin/env python3
"""ORUN PC Edge: sealed Gateway USB/BLE inbox -> authenticated backend upload.

The PC stores exact protected frames (not plaintext, not K_root) with SQLite
WAL+FULL. Another worker retries uploads if the backend or network is offline.
The backend is the only place that decrypts and authenticates Tracker data.

IMPORTANT: This bridge NEVER sends EDGE ACCEPT/reclaim. Backend STORED does
not yet constitute the Gateway's authenticated custody-release protocol.
Therefore Gateway-held pages remain held and may fill. Do not erase them.

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
    EDGE_SERVICE_UUID, EDGE_TO_EDGE_UUID, EdgeReassembler,
)

USB_HELD = re.compile(
    r"EDGE (?:HELD|PRIORITY) v1 gw=([0-9A-F]{16}) page=\\d+ slot=\\d+ "
    r"gen=[0-9A-F]{16} len=(\\d+) obj=([0-9A-F]+)(?:\\s|$)"
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
    if len(token) < 8 or "\\n" in token or "\\r" in token:
        raise ValueError("token file must contain a single token of 8+ chars")
    return token


class Inbox:
    def __init__(self, file: Path):
        file = Path(file).expanduser()
        file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.path = file
        with self.conn() as db:
            db.executescript(SCHEMA)

    def conn(self):
        db = sqlite3.connect(str(self.path), timeout=5)
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=FULL")
        db.execute("PRAGMA busy_timeout=5000")
        return db

    def ingest(self, raw: bytes, gateway: str | None = None) -> bool:
        # This is a deliberately untrusted *sealed* queue. No AEAD validity
        # is claimed here; only the backend may authenticate and release it.
        if (len(raw) not in (84, 100) or raw[:3] != b"\\x02\\x04\\x02"
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
    raw = m[3]
    if len(raw) not in (168, 200) or len(raw) != 2 * int(m[2]):
        return False
    return inbox.ingest(bytes.fromhex(raw), m[1])


def handle_ble_message(msg: bytes, inbox: Inbox, gateway: str | None) -> str | None:
    if len(msg) == 13 and msg[0] == 1:
        return msg[1:9].hex().upper()
    if (gateway and msg and msg[0] in (2, 6) and len(msg) >= 18
            and len(msg) == 18 + msg[17] and msg[17] in (84, 100)):
        inbox.ingest(bytes(msg[18:]), gateway)
    return gateway


def usb_forever(inbox: Inbox, port: str) -> None:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("Install python3-serial (pyserial) for USB mode") from exc
    while True:
        try:
            with serial.Serial(port, 115200, timeout=0.5) as ser:
                print("[usb] Gateway connected:", port, flush=True)
                last_dump = 0.
                while True:
                    now = time.monotonic()
                    if now - last_dump >= 120:
                        ser.write(b"BENCH GW DUMP\\n")  # READ-ONLY
                        last_dump = now
                    raw = ser.read_until(b"\\n", size=4096)
                    if raw and handle_usb_line(raw.decode("ascii", "replace"), inbox):
                        print("[usb] sealed record committed", inbox.counts(), flush=True)
        except (OSError, serial.SerialException) as exc:
            print("[usb] disconnected; reconnecting:", type(exc).__name__, flush=True)
            time.sleep(5)


def ble_forever(inbox: Inbox, address: str | None) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise SystemExit("Install bleak/BlueZ for BLE mode") from exc

    async def run():
        while True:
            try:
                if address:
                    device = address
                else:
                    def match(dev, adv):
                        return EDGE_SERVICE_UUID in [
                            s.lower() for s in (adv.service_uuids or [])
                        ]
                    device = await BleakScanner.find_device_by_filter(
                        match, timeout=20)
                if device is None:
                    await asyncio.sleep(5)
                    continue
                queue = asyncio.Queue()
                def received(_, data):
                    queue.put_nowait(bytes(data))
                async with BleakClient(device, timeout=20) as client:
                    await client.start_notify(EDGE_TO_EDGE_UUID, received)
                    print("[ble] Gateway connected", flush=True)
                    reassembler = EdgeReassembler()
                    gateway = None
                    while client.is_connected:
                        frame = await asyncio.wait_for(queue.get(), timeout=30)
                        msg = reassembler.push(frame)
                        if msg:
                            gateway = handle_ble_message(msg, inbox, gateway)
            except (Exception,) as exc:
                print("[ble] reconnecting after", type(exc).__name__, flush=True)
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
    args = ap.parse_args()
    if args.mode == "usb" and not args.port:
        ap.error("--port is required for USB mode")
    # Fail before touching local SQLite when destination/token are wrong.
    checked_backend(args.backend_url)
    read_token(args.token_file.expanduser())
    inbox = Inbox(args.db)
    uploader = Uploader(inbox, args.backend_url, args.token_file.expanduser(), args.edge_id)
    print("[pc-edge] spool:", inbox.path, inbox.counts(), flush=True)
    print("[pc-edge] no EDGE ACCEPT/reclaim: Gateway retains custody", flush=True)
    if args.mode == "upload-only":
        uploader.forever()
    else:
        threading.Thread(target=uploader.forever, daemon=True).start()
        if args.mode == "usb":
            usb_forever(inbox, args.port)
        else:
            ble_forever(inbox, args.ble_address)


if __name__ == "__main__":
    main()
