"""HTTP adapter for the ORUN SharedStore S2 candidate.

There is deliberately NO built-in authentication, default credential, server
launcher, CORS wildcard, or integration into the bench observation backend.
The caller MUST inject a Step-3C-verified principal resolver; tests can inject
a fixed fixture resolver. Authentication is not derived from a caller-sent
site/actor ID, Tailscale address, or the bench upload token.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler
import json
from typing import Callable
from urllib.parse import parse_qs, urlsplit

from .shared_sync_store import Actor, SharedStore, SyncForbidden, SyncInvalid

MAX_BODY = 128 * 1024


def handler_factory(store: SharedStore, resolve_actor: Callable[[object], Actor | None]):
    if not callable(resolve_actor):
        raise ValueError("authenticated principal resolver required")

    class Handler(BaseHTTPRequestHandler):
        server_version = "ORUN-Shared-S2"

        def log_message(self, *_args):
            # Caller owns safe request logs; never log Bearer or private payload.
            pass

        def _send(self, code: int, body: dict) -> None:
            payload = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(payload)

        def _auth(self) -> Actor | None:
            try:
                return resolve_actor(self.headers)
            except Exception:
                # Resolver errors must never become permissive.
                return None

        def do_GET(self):  # noqa: N802
            if urlsplit(self.path).path != "/api/v1/sync":
                return self._send(404, {"error": "not_found"})
            actor = self._auth()
            if actor is None:
                return self._send(401, {"error": "unauthorized"})
            try:
                params = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                if set(params) - {"cursor", "limit"} or any(len(v) != 1 for v in params.values()):
                    raise SyncInvalid("bad query")
                cursor = params.get("cursor", ["0"])[0]
                limit = params.get("limit", ["100"])[0]
                if not cursor.isascii() or not cursor.isdecimal() or not limit.isascii() or not limit.isdecimal():
                    raise SyncInvalid("bad cursor/limit")
                return self._send(200, store.pull(actor, int(cursor), int(limit)))
            except SyncForbidden:
                return self._send(403, {"error": "forbidden"})
            except (SyncInvalid, ValueError):
                return self._send(400, {"error": "invalid_query"})

        def do_POST(self):  # noqa: N802
            if urlsplit(self.path).path != "/api/v1/sync/ops" or urlsplit(self.path).query:
                return self._send(404, {"error": "not_found"})
            actor = self._auth()
            if actor is None:
                return self._send(401, {"error": "unauthorized"})
            length = self.headers.get("Content-Length", "")
            if self.headers.get("Transfer-Encoding") or not length.isascii() or not length.isdecimal() or int(length) > MAX_BODY:
                return self._send(413, {"error": "size"})
            try:
                raw = self.rfile.read(int(length))
                data = json.loads(raw.decode("utf-8"))
                if not isinstance(data, dict) or set(data) != {"ops"}:
                    raise SyncInvalid("body schema")
                results = store.mutate(actor, data["ops"])
                return self._send(200, {"results": results})
            except SyncForbidden:
                return self._send(403, {"error": "forbidden"})
            except (SyncInvalid, ValueError, UnicodeDecodeError, TypeError):
                return self._send(400, {"error": "invalid_body"})

    return Handler
