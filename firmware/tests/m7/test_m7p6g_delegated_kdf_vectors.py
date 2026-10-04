#!/usr/bin/env python3
"""Independent host KAT for the delegated gateway KDF/frame-key candidate."""

import hashlib
import hmac
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "firmware/tests/m7/m7p6g_delegated_kdf_vectors.h"
TEXT = HEADER.read_text(encoding="utf-8")


def array_bytes(name):
    match = re.search(
        rf"{name}\[[^\]]+\]\s*=\s*\{{(.*?)\}};",
        TEXT,
        flags=re.S,
    )
    assert match, f"missing array {name}"
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))


def scalar_hex(name):
    match = re.search(
        rf"{name}\s*=\s*(?:UINT64_C\()?0x([0-9A-Fa-f]+)",
        TEXT,
    )
    assert match, f"missing scalar {name}"
    return int(match.group(1), 16)


def string_value(name):
    match = re.search(rf'{name}\[\]\s*=\s*"([^"]+)";', TEXT)
    assert match, f"missing string {name}"
    return match.group(1).encode("ascii")


def hkdf_extract(salt, ikm):
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, length):
    assert 0 < length <= 255 * hashlib.sha256().digest_size
    output = bytearray()
    block = b""
    counter = 1
    while len(output) < length:
        block = hmac.new(
            prk, block + info + bytes((counter,)), hashlib.sha256
        ).digest()
        output.extend(block)
        counter += 1
    return bytes(output[:length])


root = array_bytes("kRoot")
credential_id = array_bytes("kCredentialId")
frame_key_salt = array_bytes("kFrameKeySalt")

key_epoch = scalar_hex("kKeyEpoch")
gateway_device_id = scalar_hex("kGatewayDeviceId")
policy_floor = scalar_hex("kGatewayPolicyFloor")
grant_generation = scalar_hex("kGatewayGrantGeneration")
scope_id = scalar_hex("kScopeId")
quota_code = scalar_hex("kQuotaCode")
security_counter = scalar_hex("kSecurityCounter")
context_gw2d = scalar_hex("kContextGw2d")
context_d2gw = scalar_hex("kContextD2gw")

assert context_gw2d == 0x03
assert context_d2gw == 0x04
assert 0 <= scope_id <= 0x0F
assert 0 <= quota_code <= 0x03

prk = hkdf_extract(credential_id, root)
assert prk == array_bytes("kExpectedPrk")

grant_info = (
    string_value("kGrantLabel")
    + key_epoch.to_bytes(4, "big")
    + gateway_device_id.to_bytes(8, "big")
    + policy_floor.to_bytes(4, "big")
    + grant_generation.to_bytes(4, "big")
    + bytes((scope_id, quota_code))
)
assert len(grant_info) == 45
assert grant_info == array_bytes("kExpectedGrantInfo")

grant_key = hkdf_expand(prk, grant_info, 32)
assert grant_key == array_bytes("kExpectedGrantKey")

gw2d_frame_info = (
    string_value("kFrameLabel") + bytes((context_gw2d,)) + frame_key_salt
)
d2gw_frame_info = (
    string_value("kFrameLabel") + bytes((context_d2gw,)) + frame_key_salt
)
assert len(gw2d_frame_info) == 36
assert len(d2gw_frame_info) == 36
assert gw2d_frame_info == array_bytes("kExpectedGw2dFrameInfo")
assert d2gw_frame_info == array_bytes("kExpectedD2gwFrameInfo")

gw2d_frame_key = hkdf_expand(grant_key, gw2d_frame_info, 16)
d2gw_frame_key = hkdf_expand(grant_key, d2gw_frame_info, 16)
assert gw2d_frame_key == array_bytes("kExpectedGw2dFrameKey")
assert d2gw_frame_key == array_bytes("kExpectedD2gwFrameKey")
assert gw2d_frame_key != d2gw_frame_key

gw2d_nonce = (
    key_epoch.to_bytes(4, "big")
    + bytes((context_gw2d,))
    + security_counter.to_bytes(8, "big")
)
d2gw_nonce = (
    key_epoch.to_bytes(4, "big")
    + bytes((context_d2gw,))
    + security_counter.to_bytes(8, "big")
)
assert gw2d_nonce == array_bytes("kExpectedGw2dNonce")
assert d2gw_nonce == array_bytes("kExpectedD2gwNonce")
assert gw2d_nonce != d2gw_nonce

# Binding checks: every grant field is cryptographically significant.
def derive_grant(*, epoch=key_epoch, gateway=gateway_device_id,
                 floor=policy_floor, generation=grant_generation,
                 scope=scope_id, quota=quota_code):
    info = (
        string_value("kGrantLabel")
        + epoch.to_bytes(4, "big")
        + gateway.to_bytes(8, "big")
        + floor.to_bytes(4, "big")
        + generation.to_bytes(4, "big")
        + bytes((scope, quota))
    )
    return hkdf_expand(prk, info, 32)


mutated_grants = (
    derive_grant(epoch=key_epoch ^ 1),
    derive_grant(gateway=gateway_device_id ^ 1),
    derive_grant(floor=policy_floor ^ 1),
    derive_grant(generation=grant_generation ^ 1),
    derive_grant(scope=scope_id ^ 1),
    derive_grant(quota=quota_code ^ 1),
)
assert all(candidate != grant_key for candidate in mutated_grants)

# Frame direction and salt are both bound into the child key.
other_salt = bytes((frame_key_salt[0] ^ 1,)) + frame_key_salt[1:]
other_salt_key = hkdf_expand(
    grant_key,
    string_value("kFrameLabel") + bytes((context_gw2d,)) + other_salt,
    16,
)
assert other_salt_key != gw2d_frame_key

print("M7P6G delegated KDF/frame-key host vectors: PASS")
