"""KQ05 LAB: derive purpose-separated Edge->Gateway offer key from OGK1 v2.

The OGK1 v2 Edge ACCEPT master key must have been obtained through the
TRUSTED, separately authorized enrollment flow. It is not a generic BLE
secret or the Backend->Edge key. This function does not authorize radio
traffic, Gateway STAGED response, or Tracker record deletion.
"""
from __future__ import annotations

import hashlib
import hmac
import re

_LABEL = b"ORUN-KQ05-OGK1-EDGE-LINK-DERIVE-v1\x00"
_ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z", re.ASCII)


def derive_gateway_offer_key(ogk1_edge_accept_key: bytes,
                             gateway_id: bytes,
                             trusted_edge_id: str) -> bytes:
    if (type(ogk1_edge_accept_key) is not bytes or
            len(ogk1_edge_accept_key) != 32 or
            not any(ogk1_edge_accept_key) or
            type(gateway_id) is not bytes or len(gateway_id) != 8 or
            not any(gateway_id) or
            type(trusted_edge_id) is not str or
            not _ID.fullmatch(trusted_edge_id)):
        raise ValueError("trusted enrolled OGK1 v2 key, gateway and Edge identity required")
    return hmac.new(
        ogk1_edge_accept_key,
        _LABEL + gateway_id + trusted_edge_id.encode("ascii"),
        hashlib.sha256,
    ).digest()
