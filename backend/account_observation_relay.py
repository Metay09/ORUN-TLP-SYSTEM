"""Account-authorized proxy to the existing PC Edge observation Backend.

No phone ever sees the legacy observation bearer, and users do not configure
a second server. This relay is available ONLY when an operator supplies an
existing private 0600 bearer file and a pinned local/tailnet upstream URL.
No credential is accepted from client input and no URL is client-controlled.
"""
from __future__ import annotations

import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import stat
from urllib.parse import urlsplit


class RelayConfigError(ValueError):
    pass


class RelayUnavailable(RuntimeError):
    pass


_MAX_REQUEST = 256 * 1024
_MAX_RESPONSE = 512 * 1024
_PATHS = frozenset({("POST", "/v1/edge/batch"),
                    ("GET", "/v1/edge/keys")})


class AccountObservationRelay:
    def __init__(self, upstream: str, token_file: str | Path) -> None:
        parsed = urlsplit(upstream)
        if (parsed.scheme != "http" or parsed.username or parsed.password or
                parsed.path not in ("", "/") or parsed.query or
                parsed.fragment or not parsed.hostname or not parsed.port):
            raise RelayConfigError("OBSERVATION_UPSTREAM_PRIVATE_HTTP_REQUIRED")
        try:
            address = ipaddress.ip_address(parsed.hostname)
        except ValueError:
            raise RelayConfigError("OBSERVATION_UPSTREAM_IP_REQUIRED") from None
        if address.version != 4 or not (
                address.is_loopback or address in ipaddress.ip_network(
                    "100.64.0.0/10")):
            raise RelayConfigError("OBSERVATION_UPSTREAM_NOT_LOCAL_OR_TAILNET")

        source = Path(token_file)
        info = source.lstat()
        parent = source.parent.lstat()
        if (not stat.S_ISREG(info.st_mode) or
                info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077 or
                not stat.S_ISDIR(parent.st_mode) or
                parent.st_uid != os.geteuid() or
                stat.S_IMODE(parent.st_mode) & 0o077):
            raise RelayConfigError("OBSERVATION_TOKEN_NOT_PRIVATE")
        fd = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        try:
            current = os.fstat(fd)
            if (not stat.S_ISREG(current.st_mode) or
                    current.st_uid != os.geteuid() or
                    stat.S_IMODE(current.st_mode) & 0o077):
                raise RelayConfigError("OBSERVATION_TOKEN_CHANGED")
            secret = os.read(fd, 513)
        finally:
            os.close(fd)
        token = secret.strip()
        if (len(token) < 8 or len(token) > 512 or
                not re.fullmatch(rb"[\x21-\x7e]+", token)):
            raise RelayConfigError("OBSERVATION_TOKEN_INVALID")
        self.host = str(address)
        self.port = parsed.port
        self._token = token.decode("ascii")

    def forward(self, method: str, path: str,
                data: bytes = b"") -> tuple[int, dict]:
        if (method, path) not in _PATHS:
            raise RelayUnavailable("UNSUPPORTED_ROUTE")
        if len(data) > _MAX_REQUEST:
            raise RelayUnavailable("REQUEST_TOO_LARGE")
        conn = http.client.HTTPConnection(self.host, self.port, timeout=10)
        try:
            conn.request(
                method, path, body=data if method == "POST" else None,
                headers={
                    "Authorization": "Bearer " + self._token,
                    "Accept": "application/json",
                    **({"Content-Type": "application/json"} if method == "POST"
                       else {}),
                })
            reply = conn.getresponse()
            if reply.getheader("Content-Type", "").split(";", 1)[0].strip() != "application/json":
                raise RelayUnavailable("NON_JSON_UPSTREAM")
            raw = reply.read(_MAX_RESPONSE + 1)
            if len(raw) > _MAX_RESPONSE:
                raise RelayUnavailable("UPSTREAM_TOO_LARGE")
            value = json.loads(raw.decode("utf-8"))
            if not isinstance(value, dict) or not 200 <= reply.status <= 599:
                raise RelayUnavailable("UPSTREAM_RESPONSE_INVALID")
            return reply.status, value
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as err:
            raise RelayUnavailable("OBSERVATION_BACKEND_UNAVAILABLE") from None
        finally:
            conn.close()
