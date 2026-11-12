"""Y2b Edge ACCEPT: an explicit Gateway secret; no test-key fallback.

The wire MAC still binds gateway, page, slot, generation and exact sealed bytes.
Secrets are supplied by the authenticated farm server and cached owner-only for
Internet outages. A missing key leaves custody on the Gateway.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import threading
from urllib.request import Request


def accept_mac(key: bytes, gateway: int, page: int, slot: int,
               generation: int, frame: bytes) -> bytes:
    if len(key) != 32 or not any(key):
        raise ValueError("missing Gateway Edge key")
    message = (b"ORUN-EDGE-ACCEPT-v1" + gateway.to_bytes(8, "big") +
               page.to_bytes(2, "big") + slot.to_bytes(2, "big") +
               generation.to_bytes(8, "big") + hashlib.sha256(frame).digest())
    return hmac.digest(key, message.ljust(102, b"\0"), "sha256")


def accept_line(key: bytes, gateway: int, page: int, slot: int,
                generation: int, frame: bytes) -> str:
    mac = accept_mac(key, gateway, page, slot, generation, frame)
    return f"EDGE ACCEPT v1 page={page} slot={slot} gen={generation:016X} mac={mac.hex().upper()}"


def accept_binary(key: bytes, gateway: int, page: int, slot: int,
                  generation: int, frame: bytes) -> bytes:
    return (b"\x82" + page.to_bytes(2, "big") + slot.to_bytes(2, "big") +
            generation.to_bytes(8, "big") + accept_mac(key, gateway, page, slot, generation, frame))


def valid_keys(value: object) -> dict[str, bytes]:
    if not isinstance(value, list) or len(value) > 256:
        raise ValueError("invalid Edge key list")
    keys = {}
    for row in value:
        if (not isinstance(row, dict) or not isinstance(row.get("gateway"), str) or
                not re.fullmatch(r"[0-9A-F]{16}", row["gateway"]) or
                not isinstance(row.get("key"), str) or
                not re.fullmatch(r"[0-9A-F]{64}", row["key"])):
            raise ValueError("invalid Edge key entry")
        key = bytes.fromhex(row["key"])
        if not any(key) or key == b"\xff" * 32 or row["gateway"] in keys:
            raise ValueError("invalid Edge key entry")
        keys[row["gateway"]] = key
    return keys


class EdgeKeyCache:
    def __init__(self, path: Path, server: str):
        self.path, self.server = path, server
        self.keys: dict[str, bytes] = {}
        self.lock = threading.Lock()
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        except FileNotFoundError:
            return
        with os.fdopen(fd) as f:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode) or st.st_mode & 0o077 or st.st_uid != os.getuid():
                raise ValueError("Edge key cache must be an owner-only regular file")
            data = json.loads(f.read(65537))
        if data.get("server") == server:
            self.keys = valid_keys(data.get("keys"))

    def get(self, gateway: str) -> bytes | None:
        with self.lock:
            return self.keys.get(gateway)

    def refresh(self, opener, token: str) -> None:
        req = Request(self.server + "/v1/bridge/edge-keys",
                      headers={"Authorization": "Bearer " + token})
        with opener.open(req, timeout=5) as response:
            data = json.loads(response.read(65537))
        keys = valid_keys(data.get("keys"))
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd, name = tempfile.mkstemp(prefix=".edge-keys-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w") as f:
                json.dump({"server": self.server, "keys": data["keys"]}, f)
                f.flush()
                os.fsync(f.fileno())
            os.replace(name, self.path)
            d = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(d)
            finally:
                os.close(d)
        finally:
            if os.path.exists(name):
                os.unlink(name)
        with self.lock:
            self.keys = keys
