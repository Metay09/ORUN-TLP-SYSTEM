#!/usr/bin/env python3
"""SF5G product custody *preflight model*, not a production store.

Pin the old SF4B 73-byte format and check a separate, candidate 84/100-byte
product-record layout. These tests do NOT authorize ACK, flash writes or RF.
"""

from pathlib import Path
import re
import zlib

root = Path(__file__).resolve().parents[2]
legacy = (root / "include" / "custody_store_format.h").read_text()
product = (root / "include" / "tlp_v2_product_secure.h").read_text()
delegated = (root / "include" / "tlp_v2_delegated_secure_app.h").read_text()


def pinned(assertion: str, source: str) -> None:
    assert re.search(assertion, source), f"source contract changed: {assertion}"


# Do not silently repurpose legacy SF4B OCQ1/v2 pages or claim they hold SF5B.
pinned(r"kPageMagic\s*=\s*0x4F435131U", legacy)
pinned(r"kVersion\s*=\s*2U", legacy)
pinned(r"kRecordSize\s*=\s*92U", legacy)
pinned(r"kObjectSize\s*=\s*tlp::kHistoryObservationPacketSize", legacy)
pinned(r"static_assert\(kObjectSize == 73U", legacy)
pinned(r"static_assert\(kRecordsPerPage == 43U", legacy)
pinned(r"static_assert\(kPeriodicFrameSize == 100U", product)
pinned(r"static_assert\(kEventFrameSize == 84U", product)
pinned(r"kDelegatedSecureHeaderSize\s*=\s*56U", delegated)
pinned(r"kDelegatedSecureMaxCiphertextSize\s*=\s*32U", delegated)
pinned(r"kDelegatedSecureTagSize\s*=\s*8U", delegated)
pinned(r"kPacketTypeDelegatedSecureApp\s*=\s*0x01U", delegated)
pinned(r"kSecurityContextDelegatedD2gw\s*=\s*0x04U", delegated)
pinned(r"kDelegatedAppFamilyResult\s*=\s*0x02U", delegated)
# Envelope upper bound = 56 + 32 + 8 = 96; this test does not
# yet authorize custody of that family in the 84/100-only codec.
assert 56 + 32 + 8 == 96 and 96 <= 100

# An UNAPPROVED candidate: 64-byte header, 3 x 24-byte intent slots,
# 120-byte record with 2-byte length + 2-byte reserved + 100-byte frame slot,
# 4-byte CRC + 4-byte commit + two 4-byte handoff markers.
PAGE = 4096
HEADER = 64
INTENT_SLOTS = 3
INTENT_SIZE = 24
RECORD = 120
OBJECT_SLOT = 100
RECORDS_PER_PAGE = 32
MIN_GUARD = 4
CRC_AT = 104
COMMIT_AT = 108
HANDOFF_A_AT = 112
HANDOFF_B_AT = 116

assert CRC_AT == 4 + OBJECT_SLOT
assert (CRC_AT, COMMIT_AT, HANDOFF_A_AT, HANDOFF_B_AT) == (104, 108, 112, 116)
assert HANDOFF_B_AT + 4 == RECORD
assert HEADER + RECORDS_PER_PAGE * RECORD + INTENT_SLOTS * INTENT_SIZE <= PAGE
GAP = PAGE - HEADER - RECORDS_PER_PAGE * RECORD - INTENT_SLOTS * INTENT_SIZE
assert GAP == 120 and GAP >= MIN_GUARD
assert ((PAGE - HEADER - INTENT_SLOTS * INTENT_SIZE - MIN_GUARD) // RECORD
        == RECORDS_PER_PAGE)
assert 32 * RECORDS_PER_PAGE == 1024  # nominal only; rotation reduces availability


def stage_exact_object(obj: bytes) -> bytearray:
    assert len(obj) in (84, 100)
    out = bytearray(b"\xff" * RECORD)
    out[:2] = len(obj).to_bytes(2, "big")
    out[2:4] = b"\x00\x00"
    out[4:4 + len(obj)] = obj
    out[CRC_AT:COMMIT_AT] = (zlib.crc32(out[:CRC_AT]) & 0xffffffff).to_bytes(4, "big")
    return out


def inspect_committed(raw: bytes) -> bytes | None:
    if len(raw) != RECORD or raw[COMMIT_AT:COMMIT_AT + 4] != b"\x00" * 4:
        return None
    n = int.from_bytes(raw[:2], "big")
    if n not in (84, 100) or raw[2:4] != b"\x00\x00":
        return None
    if any(b != 0xff for b in raw[4 + n:CRC_AT]):
        return None
    if int.from_bytes(raw[CRC_AT:COMMIT_AT], "big") != zlib.crc32(raw[:CRC_AT]):
        return None
    return bytes(raw[4:4 + n])


for size, family in ((84, 2), (100, 1)):
    # Synthetic headers test exact-length retention, not AEAD authentication.
    obj = bytearray((i * 13) & 0xff for i in range(size))
    obj[0:2] = b"\x02\x04"
    obj[3] = family
    obj[6] = size - 36
    staged = stage_exact_object(bytes(obj))
    assert inspect_committed(staged) is None  # pre-commit must not count as custody
    staged[COMMIT_AT:COMMIT_AT + 4] = b"\x00" * 4
    assert inspect_committed(staged) == bytes(obj)
    corrupt = bytearray(staged)
    corrupt[9] ^= 1
    assert inspect_committed(corrupt) is None
    truncated = bytearray(staged)
    truncated[:2] = b"\x00\x49"
    assert inspect_committed(truncated) is None

print("SF5G custody preflight (legacy guard + candidate geometry/model): PASS")
print(f"Candidate: {RECORDS_PER_PAGE} records/page, {GAP}-byte gap, "
      "1024 nominal slots/32 pages; not a production capacity guarantee")
