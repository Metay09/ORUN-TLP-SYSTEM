#!/usr/bin/env python3
"""Read-only ORUN observation sidecar for coexistence with the legacy map.

It exposes only authenticated GET /v1/edge/records?after=<cursor>. Unlike
orun_backend_bench.Backend, this server NEVER creates tables, opens the
credential registry, accepts uploads, issues keys, or modifies live SQLite.

Bind is restricted to loopback: account relay can reach it on the same PC.
The old Backend remains the pinned destination for upload and read-key calls.
"""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hmac
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.observation_read_page import (  # noqa: E402
    MAX_CURSOR, ObservationReadUnavailable, read_page,
)
from backend.orun_backend_bench import load_private_token_file  # noqa: E402

EXACT_RECORDS = re.compile(r"^/v1/edge/records\?after=(0|[1-9][0-9]{0,18})$")


def make_readonly_server(
    db_path: str | Path, token: str, host: str = "127.0.0.1",
    port: int = 0,
) -> ThreadingHTTPServer:
    if host != "127.0.0.1":
        raise ValueError("SIDECAR_LOOPBACK_ONLY")
    if type(port) is not int or not 0 <= port <= 65535:
        raise ValueError("SIDECAR_INVALID_PORT")
    if not isinstance(token, str) or len(token) < 8:
        raise ValueError("SIDECAR_INVALID_BEARER")
    db_path = Path(db_path)
    if not db_path.is_file():
        raise ValueError("SIDECAR_LEDGER_NOT_FOUND")
    expected = token.encode("ascii")

    class Handler(BaseHTTPRequestHandler):
        server_version = "ORUN-observation-readonly"
        def log_message(self, *_args):
            pass

        def send_json(self, status: int, body: dict) -> None:
            raw = json.dumps(body, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):  # noqa: N802
            if self.path.split("?", 1)[0] != "/v1/edge/records":
                return self.send_json(404, {"error": "not_found"})
            auth = self.headers.get("Authorization", "")
            candidate = auth[7:].encode() if auth.startswith("Bearer ") else b""
            if not hmac.compare_digest(candidate, expected):
                return self.send_json(401, {"error": "unauthorized"})
            match = EXACT_RECORDS.fullmatch(self.path)
            if match is None or int(match.group(1)) > MAX_CURSOR:
                return self.send_json(400, {"error": "invalid_cursor"})
            if self.headers.get("Transfer-Encoding") or self.headers.get("Content-Length") not in (None, "0"):
                return self.send_json(400, {"error": "invalid_request"})
            try:
                page = read_page(db_path, int(match.group(1)))
            except ObservationReadUnavailable:
                return self.send_json(503, {"error": "ledger_unavailable"})
            return self.send_json(200, page)

        def do_POST(self):  # noqa: N802
            return self.send_json(405, {"error": "readonly"})

    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    return server


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--db", required=True)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args(argv)
    try:
        token = load_private_token_file(args.token_file)
        server = make_readonly_server(args.db, token, args.bind, args.port)
    except (OSError, ValueError) as exc:
        parser.error("Read-only sidecar setup refused (path, bearer, or bind invalid)")
    print("ORUN_OBSERVATION_READONLY_BIND=127.0.0.1:" +
          str(server.server_port), flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
