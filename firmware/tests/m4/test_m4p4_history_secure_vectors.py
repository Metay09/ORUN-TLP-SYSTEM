#!/usr/bin/env python3
import hashlib
import hmac

from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out = b""
    previous = b""
    counter = 1
    while len(out) < length:
        previous = hmac.new(
            prk, previous + info + bytes([counter]), hashlib.sha256
        ).digest()
        out += previous
        counter += 1
    return out[:length]


def be(value: int, size: int) -> bytes:
    return value.to_bytes(size, "big")


K_ROOT = bytes(range(0x20))
CREDENTIAL_ID = bytes(range(0xA0, 0xB0))
LABEL = b"ORUN-TLP-V2-AEAD"
KEY_EPOCH = 0x01020304
DEVICE_ID = 0x1122334455667788
INCARNATION = 0xA1A2A3A4A5A6A7A8

PRK = hkdf_extract(CREDENTIAL_ID, K_ROOT)
D2A_KEY = hkdf_expand(PRK, LABEL + b"\x01" + be(KEY_EPOCH, 4), 16)
A2D_KEY = hkdf_expand(PRK, LABEL + b"\x02" + be(KEY_EPOCH, 4), 16)

assert D2A_KEY.hex() == "b4db25a99bade834d006c0992d6dbe1a"
assert A2D_KEY.hex() == "dea76f45a7abc04233848335b16a3ee1"

OBS_COUNTER = 0x1122334455667788
OBS_PLAINTEXT = bytes.fromhex(
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
OBS_AAD = (
    bytes.fromhex("02030101011d0000")
    + be(DEVICE_ID, 8)
    + be(KEY_EPOCH, 4)
    + be(OBS_COUNTER, 8)
    + be(INCARNATION, 8)
)
OBS_NONCE = be(KEY_EPOCH, 4) + b"\x01" + be(OBS_COUNTER, 8)
obs_result = AESCCM(D2A_KEY, tag_length=8).encrypt(
    OBS_NONCE, OBS_PLAINTEXT, OBS_AAD
)
OBS_CIPHERTEXT, OBS_TAG = obs_result[:-8], obs_result[-8:]

assert len(OBS_AAD) == 36
assert len(OBS_PLAINTEXT) == 29
assert OBS_NONCE.hex() == "01020304011122334455667788"
assert OBS_CIPHERTEXT.hex() == (
    "61ff0ec3e211ae143cbee92d895cb1631134aa08fd957b0b96093f0d66"
)
assert OBS_TAG.hex() == "fbcff9c04d2f4e81"

RECEIPT_COUNTER = 0x8877665544332211
RECEIPT_PLAINTEXT = bytes.fromhex(
    "01010200"
    "0102030405060708"
    "1112131415161718"
)
RECEIPT_AAD = (
    bytes.fromhex("0203020201140000")
    + be(DEVICE_ID, 8)
    + be(KEY_EPOCH, 4)
    + be(RECEIPT_COUNTER, 8)
    + be(INCARNATION, 8)
)
RECEIPT_NONCE = (
    be(KEY_EPOCH, 4) + b"\x02" + be(RECEIPT_COUNTER, 8)
)
receipt_result = AESCCM(A2D_KEY, tag_length=8).encrypt(
    RECEIPT_NONCE, RECEIPT_PLAINTEXT, RECEIPT_AAD
)
RECEIPT_CIPHERTEXT, RECEIPT_TAG = (
    receipt_result[:-8],
    receipt_result[-8:],
)

assert len(RECEIPT_AAD) == 36
assert len(RECEIPT_PLAINTEXT) == 20
assert RECEIPT_NONCE.hex() == "01020304028877665544332211"
assert RECEIPT_CIPHERTEXT.hex() == (
    "1977aff69f4a3ec1ef72bfd819058631140bf16a"
)
assert RECEIPT_TAG.hex() == "e28ddfd258634294"

print("M4P4 History secure D2A/A2D vectors: PASS")
