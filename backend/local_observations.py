"""In-process observation service for the single ORUN server (1a).

Same interface as AccountObservationRelay (``observation_site`` and
``forward(method, path, data) -> (status, dict)``), so the account HTTP
handler keeps all its authorization and site checks unchanged. The difference
is only that keys, records and uploads are served from this process instead
of being relayed over HTTP to a separate Backend and read-only sidecar.
"""
from __future__ import annotations

import json
import re

from .account_observation_relay import RelayUnavailable
from .observation_read_page import (
    MAX_CURSOR, ObservationReadUnavailable, read_latest, read_page,
)
# Same credentials module object the Backend itself uses.
from .orun_backend_bench import Backend, BadRequest, MAX_BODY, oc

_RECORDS = re.compile(r"^/v1/edge/records\?(?:after=(0|[1-9][0-9]{0,18})|latest=1)$")


class LocalObservations:
    def __init__(self, backend: Backend, observation_site: str) -> None:
        self.backend = backend
        self.observation_site = observation_site

    def keys(self) -> dict:
        registry = self.backend.registry
        return {"keys": [
            {"device": f"{c.device_id:016X}", "epoch": c.key_epoch,
             "traffic_key": oc.traffic_key(c).hex().upper()}
            for c in (registry.all() if registry else [])]}

    def records(self, path: str) -> tuple[int, dict]:
        match = _RECORDS.fullmatch(path)
        if match is None:
            raise RelayUnavailable("UNSUPPORTED_ROUTE")
        try:
            if match.group(1) is None:
                return 200, read_latest(self.backend.db_path)
            after = int(match.group(1))
            if after > MAX_CURSOR:
                return 400, {"error": "invalid_cursor"}
            return 200, read_page(self.backend.db_path, after)
        except ObservationReadUnavailable:
            return 503, {"error": "observation_read_unavailable"}

    def batch(self, data: bytes) -> tuple[int, dict]:
        if len(data) > MAX_BODY:
            return 413, {"error": "size"}
        try:
            return 200, self.backend.batch(json.loads(data.decode("utf-8")))
        except (ValueError, UnicodeDecodeError, BadRequest) as exc:
            return 400, {"error": f"malformed: {exc}"}

    def forward(self, method: str, path: str, data: bytes = b"") -> tuple[int, dict]:
        if method == "GET" and path == "/v1/edge/keys":
            return 200, self.keys()
        if method == "GET" and path.startswith("/v1/edge/records?"):
            return self.records(path)
        if method == "POST" and path == "/v1/edge/batch":
            return self.batch(data)
        raise RelayUnavailable("UNSUPPORTED_ROUTE")
