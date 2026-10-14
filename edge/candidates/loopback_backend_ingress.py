"""SF5H host-only loopback HTTP Edge -> Backend *staging* demonstrator.

The receiver stores opaque PRODUCT_SECURE frames in a second disk SQLite
spool; it does NOT decrypt, authenticate, authorize, logically deduplicate,
issue BACKEND_DURABLE/EDGE_DURABLE_ACCEPT, ACK, or release any record.

BIND LOOPBACK ONLY. This untrusted cleartext HTTP transport is intentionally
inappropriate for remote/network/production deployment.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
from http.client import HTTPConnection, HTTPException
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path

from edge_durable_inbox import EdgeDurableInbox, PutState, ScanState


_MARKER = "sf5h-untrusted-staging-v0"
_MAX_RESPONSE = 512


class LoopbackBackendServer(HTTPServer):
    """Test-only local process ingress. Never exposes any network listener."""

    def __init__(self, db_path: str | Path, max_objects: int = 4096):
        self.store = EdgeDurableInbox(db_path, max_objects=max_objects)
        self.next_fault: str | None = None
        super().__init__(("127.0.0.1", 0), _IngestHandler)

    @property
    def port(self) -> int:
        return int(self.server_address[1])


class _IngestHandler(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:
        pass  # No secrets/ciphertext in an ordinary access log.

    def _respond(self, code: int, payload: dict[str, object]) -> None:
        raw = json.dumps(payload, separators=(",", ":")).encode("ascii")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self) -> None:
        # There is NO cryptographic access control here; 127.0.0.1 is
        # only a local development containment boundary.
        if self.path != "/v0/stage":
            self._respond(404, {"kind": _MARKER, "status": "missing"})
            return
        raw_length = self.headers.get("Content-Length")
        if (raw_length is None or not raw_length.isascii() or
                not raw_length.isdecimal() or len(raw_length) > 3):
            self._respond(400, {"kind": _MARKER, "status": "bad-length"})
            return
        count = int(raw_length)
        if (count not in (84, 100) or
                self.headers.get("Content-Type") != "application/octet-stream" or
                self.headers.get("Transfer-Encoding") is not None):
            self._respond(400, {"kind": _MARKER, "status": "bad-request"})
            return
        data = self.rfile.read(count)
        if len(data) != count:
            self._respond(400, {"kind": _MARKER, "status": "truncated"})
            return

        server = self.server
        assert isinstance(server, LoopbackBackendServer)
        fault = server.next_fault
        server.next_fault = None
        result = server.store.put(data, fault=fault)
        if result.state in (PutState.COMMITTED, PutState.DUPLICATE):
            # An exact echo of a local staging fact only; not an AUTHENTICATED
            # BACKEND_DURABLE receipt or authority to delete ANY data.
            self._respond(
                201 if result.state == PutState.COMMITTED else 200,
                {"kind": _MARKER,
                 "status": "inserted" if result.state == PutState.COMMITTED
                          else "duplicate",
                 "sha256": sha256(data).hexdigest(), "size": len(data)},
            )
        elif result.state == PutState.FULL:
            self._respond(507, {"kind": _MARKER, "status": "full"})
        elif result.state in (PutState.INVALID, PutState.REJECTED):
            self._respond(422, {"kind": _MARKER, "status": "refused"})
        else:
            self._respond(503, {"kind": _MARKER, "status": "uncertain"})


class TransferState(Enum):
    STAGED_UNTRUSTED = auto()  # NOT backend durable acceptance
    EMPTY = auto()
    SOURCE_REFUSED = auto()
    SOURCE_UNAVAILABLE = auto()
    BAD_INPUT = auto()
    REMOTE_FAILED = auto()
    REMOTE_UNTRUSTED_RESPONSE = auto()


@dataclass(frozen=True)
class TransferReport:
    state: TransferState
    attempted: int = 0
    staged_untrusted: int = 0
    next_volatile_cursor: int = 0


class LoopbackEdgeSender:
    """Bounded Edge reader + real loopback HTTP post; no release API."""

    def __init__(self, source: EdgeDurableInbox, host: str, port: int):
        if (host != "127.0.0.1" or type(port) is not int or
                not 0 < port <= 65535):
            raise ValueError("Only an explicit IPv4 loopback host is allowed")
        self.source = source
        self.port = port

    def _send_one(self, frame: bytes) -> TransferState:
        try:
            conn = HTTPConnection("127.0.0.1", self.port, timeout=1.0)
            try:
                conn.request(
                    "POST", "/v0/stage", body=frame,
                    headers={"Content-Type": "application/octet-stream"},
                )
                response = conn.getresponse()
                data = response.read(_MAX_RESPONSE + 1)
                if response.status not in (200, 201):
                    return TransferState.REMOTE_FAILED
                if len(data) > _MAX_RESPONSE:
                    return TransferState.REMOTE_UNTRUSTED_RESPONSE
                message = json.loads(data)
                expected_status = "inserted" if response.status == 201 else "duplicate"
                if (not isinstance(message, dict) or
                        message.get("kind") != _MARKER or
                        message.get("status") != expected_status or
                        message.get("size") != len(frame) or
                        message.get("sha256") != sha256(frame).hexdigest()):
                    return TransferState.REMOTE_UNTRUSTED_RESPONSE
                # Still only an UNAUTHENTICATED diagnostic staging response.
                return TransferState.STAGED_UNTRUSTED
            finally:
                conn.close()
        except (OSError, HTTPException, TimeoutError, ValueError, UnicodeError,
                json.JSONDecodeError):
            return TransferState.REMOTE_FAILED

    def send_window(self, after_sequence: int = 0,
                    limit: int = 16) -> TransferReport:
        review = self.source.oldest_batch_after(after_sequence, limit)
        if review.state == ScanState.INVALID:
            return TransferReport(TransferState.BAD_INPUT,
                                  next_volatile_cursor=after_sequence)
        if review.state == ScanState.UNAVAILABLE:
            return TransferReport(TransferState.SOURCE_UNAVAILABLE,
                                  next_volatile_cursor=after_sequence)
        if review.state == ScanState.REFUSED:
            return TransferReport(TransferState.SOURCE_REFUSED,
                                  next_volatile_cursor=after_sequence)
        if review.state == ScanState.EMPTY:
            return TransferReport(TransferState.EMPTY,
                                  next_volatile_cursor=after_sequence)

        cursor = after_sequence
        attempted = 0
        staged = 0
        for item in review.items:
            attempted += 1
            state = self._send_one(item.ciphertext)
            if state != TransferState.STAGED_UNTRUSTED:
                return TransferReport(state, attempted, staged, cursor)
            staged += 1
            cursor = item.sequence  # volatile local scan position ONLY
        return TransferReport(TransferState.STAGED_UNTRUSTED, attempted, staged,
                              cursor)

# Absolutely no Edge delete, backend acceptance receipt, or Gateway reclaim.
