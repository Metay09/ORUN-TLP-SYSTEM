#!/usr/bin/env python3
"""Independent SF5B PRODUCT_SECURE vectors (ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md).

Builds the PERIODIC and EVENT plaintexts field by field with struct.pack (not
with the firmware codec), seals them with the profile-0x01 root D2A
construction (HKDF-SHA256 + AES-128-CCM, 8-byte tag) and pins the bytes. The
same hex strings are the golden inputs of sf5b_product_vectors.h, used by the
host codec test and by the RAK4631 KAT, which must reproduce the KAT frames
with CC310 through ProductSecureCrypto.
"""
import calendar
import hashlib
import hmac
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out, previous, counter = b"", b"", 1
    while len(out) < length:
        previous = hmac.new(prk, previous + info + bytes([counter]),
                            hashlib.sha256).digest()
        out += previous
        counter += 1
    return out[:length]


# Same test credential as the M4P4/SF2 HISTORY_SECURE vectors.
K_ROOT = bytes(range(0x20))
CREDENTIAL_ID = bytes(range(0xA0, 0xB0))
KEY_EPOCH = 0x01020304
DEVICE_ID = 0x1122334455667788
D2A_KEY = hkdf_expand(hkdf_extract(CREDENTIAL_ID, K_ROOT),
                      b"ORUN-TLP-V2-AEAD" + b"\x01" + KEY_EPOCH.to_bytes(4, "big"),
                      16)
assert D2A_KEY.hex() == "b4db25a99bade834d006c0992d6dbe1a"

INCARNATION = 0xA1A2A3A4A5A6A7A8
PERIOD_END = calendar.timegm((2026, 10, 11, 15, 0, 0))
assert PERIOD_END == 1791730800

# Section 7: all seven validity bits, GNSS fix, PARTIAL activity (3300 of 3600 s),
# measured battery, GNSS_DEGRADED health with one GNSS failure.
PERIODIC = struct.pack(
    ">BBBBQIIIiiiHHBBBBIIHHHHBBBB",
    0x01, 0x7F, 1, 1, INCARNATION, 257, PERIOD_END, 3600,
    376102718, 280545028, 125000, 186, 12, 5, 1, 1, 0,
    3300, 900, 14, 37, 4012, 0x0001, 1, 0, 0, 0)
assert len(PERIODIC) == 64
assert PERIODIC.hex() == (
    "017f0101a1a2a3a4a5a6a7a8000001016acba47000000e10166adf3e10b8c704"
    "0001e84800ba000c0501010000000ce400000384000e00250fac000101000000")

# Section 8: GEOFENCE_OUTSIDE ACTIVE, WARNING, time + location + age, no context.
EVENT = struct.pack(
    ">BBBBBBBBQIIQiiHHI",
    0x01, 0x01, 0x01, 0x02, 0x07, 0x00, 1, 0, INCARNATION, 258,
    PERIOD_END - 600, 0xB1B2B3B4B5B6B7B8, 376150000, 280600000, 30, 0, 0)
assert len(EVENT) == 48
assert EVENT.hex() == (
    "0101010207000100a1a2a3a4a5a6a7a8000001026acba218b1b2b3b4b5b6b7b8"
    "166b97f010b99dc0001e000000000000")


def seal(app_family: int, plaintext: bytes, counter: int) -> bytes:
    # Section 3.1 header; AAD = bytes 0..27 (RELAY_ALLOWED | CUSTODY_REQUESTED).
    header = struct.pack(">BBBBBBBBQIQ", 0x02, 0x04, 0x02, app_family, 0x03,
                         0x01, len(plaintext), 0x00, DEVICE_ID, KEY_EPOCH,
                         counter)
    assert len(header) == 28
    nonce = KEY_EPOCH.to_bytes(4, "big") + b"\x01" + counter.to_bytes(8, "big")
    return header + AESCCM(D2A_KEY, tag_length=8).encrypt(nonce, plaintext, header)


PERIODIC_FRAME = seal(0x01, PERIODIC, 0x42)
assert len(PERIODIC_FRAME) == 100
assert PERIODIC_FRAME.hex() == (
    "0204020103014000112233445566778801020304000000000000004"
    "20a19fe1a180d483149c11e413e3944c95bf8152d9d888ad7b4f16fc0bd0732"
    "07543030490747856b73c6294bec5cac9962ef666a45aeed52a37e70c12790c0"
    "e5283b12ba5d38d03f")

EVENT_FRAME = seal(0x02, EVENT, 0x43)
assert len(EVENT_FRAME) == 84
assert EVENT_FRAME.hex() == (
    "0204020203013000112233445566778801020304000000000000004"
    "3bbd99aa262fc4aed44203fb2c0a9e6e32815ee72f5b0a03dcc34a22b153b66"
    "ea9269a9a8428c27dbaebfcf615154d9158e90b4be8a6887f5")

# RAK4631 KAT frames (sf5b_product_crypto_probe.cpp): a freshly provisioned
# SecurityStore burns counter 0, so the first two protected records use
# counters 1 and 2 with the same plaintexts and path flags.
KAT_PERIODIC_FRAME = seal(0x01, PERIODIC, 1)
assert KAT_PERIODIC_FRAME.hex() == (
    "02040201030140001122334455667788010203040000000000000001"
    "e50cbc0f761c6a0eb03b458063cb18b8f077576bc1a2a7e4768c6838f05cbcce"
    "b5e1c5ebea866295c57085068185f0f6a698df279a8a7d2b5e9f9b7beda0a3f9"
    "b3417dfed60d254d")
KAT_EVENT_FRAME = seal(0x02, EVENT, 2)
assert KAT_EVENT_FRAME.hex() == (
    "02040202030130001122334455667788010203040000000000000002"
    "527c4362b1049d40724479242eff9cbc3746611b09d93b737d325ac3b884ea99"
    "96b2d4c7019183c5cc8562073586d139598a349b548bf7c6")

# Authentication binds the visible header: one changed AAD byte must fail.
tampered = bytearray(PERIODIC_FRAME)
tampered[4] ^= 0x01  # path_flags
nonce = KEY_EPOCH.to_bytes(4, "big") + b"\x01" + (0x42).to_bytes(8, "big")
try:
    AESCCM(D2A_KEY, tag_length=8).decrypt(nonce, bytes(tampered[28:]),
                                          bytes(tampered[:28]))
    raise AssertionError("tampered AAD authenticated")
except Exception as error:  # cryptography raises InvalidTag
    assert type(error).__name__ == "InvalidTag"

print("SF5B PRODUCT_SECURE independent PERIODIC/EVENT vectors: PASS")
