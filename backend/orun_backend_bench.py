#!/usr/bin/env python3
"""SF5L8 bench backend: the Edge (phone) uploads exact sealed records here.

Edge -> Backend: the phone sends each record's EXACT PRODUCT_SECURE frame
(it does not need to, and in production must not, decrypt it), plus what it
observed locally (first arrival time at the Edge, the Gateway's RX signal) and
the periods it was attached to a Gateway. This server opens every frame with
the backend verified ledger (backend/candidates/product_secure_verified_ledger
.py: AES-CCM authentication, canonical content, dedupe, conflict quarantine,
readback after COMMIT) and answers per record:

  STORED / DUPLICATE  durable here; the Edge may mark the record uploaded
  CONFLICT            same identity, different content: kept as evidence here
  REJECTED            not an authentic record for a known credential
  UNCERTAIN           storage problem: the Edge sends it again later

It serves the same field map as the PC tool (tools/edge_map) over all
uploaded records: GET / and GET /api/points.

BENCH ONLY:
- credentials: the step 3A registry (--registry, real per-Tracker keys) and,
  for old bench records only, the PUBLIC SF5L0 test credential
  (--bench-test-key);
- access is a shared bearer token (--token) over a private network (bind to
  the Tailscale address with --host). Edge identity/authorization is step 3.

Example:
  python3 backend/orun_backend_bench.py --bench-test-key --token ORUN-BENCH \\
      --host (tailscale ip -4) --port 8090
"""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hmac
import json
from pathlib import Path
import re
import stat
import sqlite3
import sys
import threading
import time
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import orun_edge_bench as eb  # noqa: E402  (ledger loader, map page, points)
import orun_credentials as oc  # noqa: E402  (step 3A registry)

MAX_BODY = 256 * 1024
MAX_RECORDS = 200
MAX_SESSIONS = 64
HEX = re.compile(r"^[0-9A-Fa-f]*$")
ID16 = re.compile(r"^[0-9A-Fa-f]{16}$")
EDGE_ID = re.compile(r"^[A-Za-z0-9._-]{1,64}$")

# Same tables the PC tool's map reads (points_from_db), plus who sent what.
SCHEMA = (
    "CREATE TABLE IF NOT EXISTS edge_receipts ("
    " device_id BLOB NOT NULL, incarnation BLOB NOT NULL, sequence INTEGER NOT NULL,"
    " first_seen_utc INTEGER NOT NULL, gw_rssi INTEGER, gw_snr INTEGER,"
    " PRIMARY KEY (device_id,incarnation,sequence))",
    "CREATE TABLE IF NOT EXISTS edge_sessions ("
    " started_utc INTEGER NOT NULL, alive_utc INTEGER NOT NULL,"
    " edge_id TEXT NOT NULL DEFAULT '', session_id INTEGER NOT NULL DEFAULT 0,"
    " transport TEXT)",
    "CREATE UNIQUE INDEX IF NOT EXISTS edge_sessions_key ON edge_sessions(edge_id, session_id)",
    "CREATE TABLE IF NOT EXISTS backend_uploads ("
    " device_id BLOB NOT NULL, incarnation BLOB NOT NULL, sequence INTEGER NOT NULL,"
    " edge_id TEXT NOT NULL, gateway TEXT, uploaded_utc INTEGER NOT NULL,"
    " PRIMARY KEY (device_id,incarnation,sequence,edge_id))",
)


class BadRequest(Exception):
    pass


class Backend:
    def __init__(self, db_path: Path, test_key: bool, clock=time.time,
                 registry: Optional[oc.CredentialRegistry] = None):
        self.mod = eb.load_ledger_module()
        self.registry = registry
        bench = eb.bench_credentials(self.mod) if test_key else eb.no_credentials
        real = registry.lookup(self.mod) if registry else eb.no_credentials

        def creds(device: bytes, epoch: int):
            # Step 3A: real credentials first; the public test key only for
            # old bench records, and only with --bench-test-key.
            return real(device, epoch) or bench(device, epoch)
        self.creds = creds
        self.ledger = self.mod.VerifiedLocalLedger(db_path, self.creds)
        self.db_path = Path(db_path)
        self.clock = clock
        self.lock = threading.Lock()   # one batch at a time, in order
        with sqlite3.connect(str(self.db_path)) as db:
            for sql in SCHEMA:
                db.execute(sql)

    @staticmethod
    def _int(v, lo: int, hi: int, name: str, optional: bool = False) -> Optional[int]:
        if v is None and optional:
            return None
        if type(v) is not int or not lo <= v <= hi:
            raise BadRequest(name)
        return v

    def batch(self, body: dict) -> dict:
        if not isinstance(body, dict):
            raise BadRequest("body")
        edge = body.get("edge")
        if not isinstance(edge, str) or not EDGE_ID.match(edge):
            raise BadRequest("edge")
        records = body.get("records", [])
        sessions = body.get("sessions", [])
        if (not isinstance(records, list) or not isinstance(sessions, list) or
                len(records) > MAX_RECORDS or len(sessions) > MAX_SESSIONS):
            raise BadRequest("records/sessions")
        parsed = []
        for r in records:
            if not isinstance(r, dict):
                raise BadRequest("record")
            frame = r.get("frame")
            if (not isinstance(frame, str) or not HEX.match(frame) or
                    len(frame) not in (168, 200)):
                raise BadRequest("frame")
            gw = r.get("gateway")
            if gw is not None and (not isinstance(gw, str) or not ID16.match(gw)):
                raise BadRequest("gateway")
            parsed.append((bytes.fromhex(frame), gw.upper() if gw else None,
                           self._int(r.get("first_seen_utc"), 1, 2**32, "first_seen_utc", True),
                           self._int(r.get("rssi"), -200, 50, "rssi", True),
                           self._int(r.get("snr"), -50, 50, "snr", True)))
        sess = []
        for s in sessions:
            if not isinstance(s, dict):
                raise BadRequest("session")
            sid = self._int(s.get("id"), 1, 2**31, "session id")
            start = self._int(s.get("started_utc"), 1, 2**32, "started_utc")
            alive = self._int(s.get("alive_utc"), 1, 2**32, "alive_utc")
            if alive < start:
                raise BadRequest("session times")
            transport = s.get("transport")
            if transport not in ("ble", "usb", None):
                raise BadRequest("transport")
            sess.append((sid, start, alive, transport))

        with self.lock:
            now = int(self.clock())
            results = []
            S = self.mod.LedgerState
            word = {S.VERIFIED_STORED: "STORED", S.VERIFIED_DUPLICATE: "DUPLICATE",
                    S.CONTENT_CONFLICT: "CONFLICT", S.REJECTED: "REJECTED",
                    S.UNCERTAIN: "UNCERTAIN"}
            for raw, gw, seen, rssi, snr in parsed:
                state = word[self.ledger.ingest(raw).state]
                if state in ("STORED", "DUPLICATE", "CONFLICT"):
                    try:
                        self._facts(raw, edge, gw, seen, rssi, snr, now)
                    except sqlite3.Error:
                        pass   # map facts only; the record itself is committed
                results.append(state)
            try:
                with sqlite3.connect(str(self.db_path), timeout=2.0) as db:
                    for sid, start, alive, transport in sess:
                        db.execute(
                            "INSERT INTO edge_sessions (started_utc,alive_utc,edge_id,session_id,transport) "
                            "VALUES (?,?,?,?,?) ON CONFLICT(edge_id,session_id) DO UPDATE SET "
                            "alive_utc=max(alive_utc,excluded.alive_utc)",
                            (start, alive, edge, sid, transport))
                sessions_ok = True
            except sqlite3.Error:
                sessions_ok = False
            return {"results": results, "sessions_ok": sessions_ok,
                    "records": self.ledger.record_count()}

    def _facts(self, raw: bytes, edge: str, gw, seen, rssi, snr, now: int) -> None:
        item = self.mod.open_product_frame(raw, self.creds)
        key = (item.device_id, item.incarnation, item.sequence)
        with sqlite3.connect(str(self.db_path), timeout=2.0) as db:
            if seen is not None:
                # Earliest arrival at any Edge; the first known RX signal.
                db.execute(
                    "INSERT INTO edge_receipts VALUES (?,?,?,?,?,?) "
                    "ON CONFLICT(device_id,incarnation,sequence) DO UPDATE SET "
                    "first_seen_utc=min(first_seen_utc,excluded.first_seen_utc), "
                    "gw_rssi=coalesce(gw_rssi,excluded.gw_rssi), "
                    "gw_snr=coalesce(gw_snr,excluded.gw_snr)",
                    key + (seen, rssi, snr))
            db.execute("INSERT OR IGNORE INTO backend_uploads VALUES (?,?,?,?,?,?)",
                       key + (edge, gw, now))


def make_server(backend: Backend, host: str, port: int, token: str) -> ThreadingHTTPServer:
    secret = token.encode()

    def authorized(headers) -> bool:
        auth = headers.get("Authorization", "")
        given = auth[7:].encode() if auth.startswith("Bearer ") else b""
        return hmac.compare_digest(given, secret)

    class Handler(BaseHTTPRequestHandler):
        server_version = "ORUN-backend-bench"

        def log_message(self, *_args):
            pass

        def _send(self, code: int, body: bytes, ctype: str, cache: bool = False) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=3600" if cache else "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code: int, obj: dict) -> None:
            self._send(code, json.dumps(obj, separators=(",", ":")).encode(), "application/json")

        def do_GET(self):  # noqa: N802
            path = self.path.split("?", 1)[0]
            if path == "/api/points":
                return self._send(200, json.dumps(eb.points_from_db(backend.db_path)).encode(),
                                  "application/json")
            if path == "/v1/edge/keys":
                # Owner decision D1: the owner's phone gets the per-epoch read
                # (D2A traffic) keys of the Trackers, never K_root. Bench: one
                # owner, the same bearer token as uploads (per-phone enrollment
                # is step 3C).
                if not authorized(self.headers):
                    return self._json(401, {"error": "token"})
                keys = [{"device": f"{c.device_id:016X}", "epoch": c.key_epoch,
                         "traffic_key": oc.traffic_key(c).hex().upper()}
                        for c in (backend.registry.all() if backend.registry else [])]
                return self._json(200, {"keys": keys})
            if path == "/v1/health":
                return self._json(200, {"ok": True, "records": backend.ledger.record_count()})
            entry = eb.STATIC_FILES.get(path)
            if entry is None:
                return self._send(404, b"not found", "text/plain")
            name, ctype = entry
            return self._send(200, (eb.MAP_DIR / name).read_bytes(), ctype, name != "index.html")

        def do_POST(self):  # noqa: N802
            if self.path != "/v1/edge/batch":
                return self._json(404, {"error": "not found"})
            if not authorized(self.headers):
                return self._json(401, {"error": "token"})
            length = self.headers.get("Content-Length", "")
            if not length.isdigit() or int(length) > MAX_BODY:
                return self._json(413, {"error": "size"})
            raw = self.rfile.read(int(length))
            try:
                body = json.loads(raw.decode("utf-8"))
                out = backend.batch(body)
            except (ValueError, UnicodeDecodeError, BadRequest) as exc:
                return self._json(400, {"error": f"malformed: {exc}"})
            return self._json(200, out)

    return ThreadingHTTPServer((host, port), Handler)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", default=str(Path.home() / "orun-backend-bench.sqlite"),
                    help="backend ledger (default: ~/orun-backend-bench.sqlite)")
    ap.add_argument("--registry", default=str(Path.home() / "orun-credentials.sqlite"),
                    help="step 3A credential registry (tools/orun_provision.py); "
                         "default ~/orun-credentials.sqlite")
    ap.add_argument("--bench-test-key", action="store_true",
                    help="accept the PUBLIC SF5L0 test credential (bench only)")
    auth = ap.add_mutually_exclusive_group(required=True)
    auth.add_argument("--token", help="shared bearer token (legacy interactive use; visible in process arguments)")
    auth.add_argument("--token-file", type=Path, help="owner-only (0600) file containing the same Edge bearer token")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (use the Tailscale address to reach it from the phone)")
    ap.add_argument("--port", type=int, default=8090)
    args = ap.parse_args(argv)
    token = args.token
    if args.token_file is not None:
        try:
            meta = args.token_file.stat()
            if not stat.S_ISREG(meta.st_mode) or (stat.S_IMODE(meta.st_mode) & 0o077):
                ap.error("--token-file must be a private regular file (chmod 600)")
            token = args.token_file.read_text(encoding="utf-8").strip()
        except (OSError, UnicodeError) as exc:
            ap.error(f"--token-file cannot be read: {type(exc).__name__}")
    if token is None or len(token) < 8 or "\\n" in token or "\\r" in token:
        ap.error("--token/--token-file: token must be at least 8 characters, one line")
    registry = oc.CredentialRegistry(args.registry)
    backend = Backend(Path(args.db), args.bench_test_key, registry=registry)
    server = make_server(backend, args.host, args.port, token)
    print(f"[sunucu] http://{args.host}:{server.server_address[1]}/  kayıt={backend.ledger.record_count()} "
          f"(çıkış Ctrl+C)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
