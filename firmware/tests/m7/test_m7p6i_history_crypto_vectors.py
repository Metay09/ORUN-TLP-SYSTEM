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

# SecurityStore's historical initial TX range begins at zero, while frozen
# HISTORY_SECURE rejects zero. M7P6I burns zero and protects with the first
# usable value, counter=1.
COUNTER = 1

PRK = hkdf_extract(CREDENTIAL_ID, K_ROOT)
D2A_KEY = hkdf_expand(
    PRK, LABEL + b"\x01" + be(KEY_EPOCH, 4), 16
)
assert D2A_KEY.hex() == "b4db25a99bade834d006c0992d6dbe1a"

PLAINTEXT = bytes.fromhex(
    "01"
    "0102030405060708"
    "11223344"
    "01020304"
    "ffffffff"
    "05060708"
    "1234"
    "08"
    "07"
)

AAD = (
    bytes.fromhex("02030201011d0000")
    + be(DEVICE_ID, 8)
    + be(KEY_EPOCH, 4)
    + be(COUNTER, 8)
    + be(INCARNATION, 8)
)
NONCE = be(KEY_EPOCH, 4) + b"\x01" + be(COUNTER, 8)

result = AESCCM(D2A_KEY, tag_length=8).encrypt(
    NONCE, PLAINTEXT, AAD
)
ciphertext, tag = result[:-8], result[-8:]
frame = AAD + ciphertext + tag

assert len(AAD) == 36
assert len(PLAINTEXT) == 29
assert NONCE.hex() == "01020304010000000000000001"
assert ciphertext.hex() == (
    "e572bf0dd3bbcfad1d8cc01b27ca1bba9e430ce43ea7aff368f4830ee7"
)
assert tag.hex() == "1ff237bdcc5c9110"
assert frame.hex() == (
    "02030201011d00001122334455667788010203040000000000000001"
    "a1a2a3a4a5a6a7a8"
    "e572bf0dd3bbcfad1d8cc01b27ca1bba9e430ce43ea7aff368f4830ee7"
    "1ff237bdcc5c9110"
)

print("M7P6I SecurityStore-owned D2A vector: PASS")
