#!/usr/bin/env python3
import hashlib
import hmac

from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out = b""
    previous = b""
    block = 1
    while len(out) < length:
        previous = hmac.new(
            prk, previous + info + bytes([block]), hashlib.sha256
        ).digest()
        out += previous
        block += 1
    return out[:length]


def be(value: int, size: int) -> bytes:
    return value.to_bytes(size, "big")


K_ROOT = bytes(range(0x20))
CREDENTIAL_ID = bytes(range(0xA0, 0xB0))
LABEL = b"ORUN-TLP-V2-AEAD"
KEY_EPOCH = 0x01020304
DEVICE_ID = 0x1122334455667788
INCARNATION = 0xA1A2A3A4A5A6A7A8
COUNTER = 1
HISTORY_RECORD_ID = 1

prk = hkdf_extract(CREDENTIAL_ID, K_ROOT)
a2d_key = hkdf_expand(
    prk, LABEL + b"\x02" + be(KEY_EPOCH, 4), 16
)
assert a2d_key.hex() == "dea76f45a7abc04233848335b16a3ee1"

plaintext = (
    b"\x01"  # receipt schema
    + b"\x01"  # BACKEND_DURABLE scope
    + b"\x01"  # one explicit identity
    + b"\x00"  # reserved
    + be(HISTORY_RECORD_ID, 8)
)

aad = (
    bytes.fromhex("02030102000c0000")
    + be(DEVICE_ID, 8)
    + be(KEY_EPOCH, 4)
    + be(COUNTER, 8)
    + be(INCARNATION, 8)
)
nonce = be(KEY_EPOCH, 4) + b"\x02" + be(COUNTER, 8)

result = AESCCM(a2d_key, tag_length=8).encrypt(
    nonce, plaintext, aad
)
frame = aad + result

assert len(frame) == 56
assert nonce.hex() == "01020304020000000000000001"
assert plaintext.hex() == "010101000000000000000001"
assert frame.hex() == (
    "02030102000c00001122334455667788010203040000000000000001"
    "a1a2a3a4a5a6a7a8"
    "1862eea2327080319fee076b2478244f78caf891"
)

print("M4P5C opaque receipt A2D vector: PASS")
