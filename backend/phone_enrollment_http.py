"""ORUN mobile registration HTTP endpoints for the shared-data backend.

Opt-in on the loopback-only shared Backend (TLS termination by a separately
configured authenticated network proxy). No account sign-in endpoint, no
remote approval endpoint, no auto-granted authority and no bench-token reuse.

An untrusted caller may begin and finish only an *owner-approved* challenge.
The trusted local operator must compare the independently displayed request
ID + key fingerprint and explicitly approve through the private CLI.
"""
from __future__ import annotations

import base64
import binascii
import hashlib
import json
import re
import sqlite3
from urllib.parse import urlsplit

from .edge_enrollment import (
    ALGORITHMS, EdgeEnrollment, EnrollmentError, EnrollmentRefused,
    MAX_SESSION_SECONDS, _validated_public_key,
)
from .shared_sync_http import handler_factory
from .shared_sync_store import SharedStore

MAX_ENROLL_BODY = 4096
REQUEST_ID = re.compile(r"^[0-9a-f]{32}$")
B64 = re.compile(r"^[A-Za-z0-9+/]*={0,2}$")


def _decode(value: object, max_encoded: int) -> bytes:
    if (not isinstance(value, str) or not 1 <= len(value) <= max_encoded
            or not B64.fullmatch(value)):
        raise ValueError("invalid encoded public key or signature")
    try:
        decoded = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError("invalid base64") from exc
    if base64.b64encode(decoded).decode("ascii") != value:
        raise ValueError("noncanonical base64")
    return decoded


def mobile_handler_factory(store: SharedStore, registry: EdgeEnrollment):
    """Extends authenticated shared sync with *opt-in* untrusted enrollment."""
    Parent = handler_factory(store, registry.resolve_headers)

    class MobileHandler(Parent):
        def do_POST(self):  # noqa: N802
            target = urlsplit(self.path)
            if target.path not in ("/api/v1/enroll/begin", "/api/v1/enroll/finish"):
                return super().do_POST()
            if target.query or target.fragment:
                return self._send(404, {"error": "not_found"})
            size = self.headers.get("Content-Length", "")
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            if (content_type != "application/json" or
                    self.headers.get("Transfer-Encoding") or not size.isascii() or
                    not size.isdecimal() or not 1 <= int(size) <= MAX_ENROLL_BODY):
                return self._send(400, {"error": "invalid_request"})
            try:
                body = json.loads(self.rfile.read(int(size)).decode("utf-8"))
                if not isinstance(body, dict):
                    raise ValueError("object required")
                algorithm = body.get("algorithm")
                if algorithm not in ALGORITHMS:
                    raise ValueError("unsupported key algorithm")
                public = _decode(body.get("public_key_base64"), 256)
                _validated_public_key(public, algorithm)
                installation = "edge-" + hashlib.sha256(public).hexdigest()
                if target.path == "/api/v1/enroll/begin":
                    if set(body) != {"algorithm", "public_key_base64"}:
                        raise ValueError("begin schema")
                    pending = registry.begin(installation, public,
                                             algorithm=algorithm)
                    return self._send(200, {
                        "installation_id": installation,
                        "request_id": pending.request_id,
                        "challenge_base64": base64.b64encode(
                            pending.challenge).decode("ascii"),
                        "fingerprint": pending.fingerprint,
                        "algorithm": algorithm,
                        "expires_at": pending.expires_at,
                    })
                if set(body) != {
                    "algorithm", "public_key_base64", "request_id",
                    "signature_base64", "owner_code"
                }:
                    raise ValueError("finish schema")
                req = body["request_id"]
                if not isinstance(req, str) or not REQUEST_ID.fullmatch(req):
                    raise ValueError("request id")
                signature = _decode(body["signature_base64"], 120)
                # The owner code is a one-time high-entropy secret approved
                # locally. Never log it or echo it in an error payload.
                token = registry.finish(
                    req, installation, public, signature, body["owner_code"],
                    algorithm=algorithm)
                return self._send(200, {
                    "installation_id": installation,
                    "access_token": token, "expires_in_seconds": MAX_SESSION_SECONDS,
                })
            except (EnrollmentRefused,) as exc:
                message = str(exc)
                if "capacity reached" in message:
                    return self._send(429, {"error": "pending_capacity"})
                return self._send(403, {"error": "not_approved_or_invalid"})
            except (EnrollmentError, ValueError, TypeError, UnicodeDecodeError):
                return self._send(400, {"error": "invalid_request"})
            except sqlite3.Error:
                return self._send(503, {"error": "temporarily_unavailable"})

    return MobileHandler
