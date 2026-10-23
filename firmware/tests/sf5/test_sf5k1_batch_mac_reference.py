#!/usr/bin/env python3
"""SF5K1 host-only proposal oracle, NOT a production ACK verifier.

The proposed 0x06 wire type, domain label and 16-byte tag are UNAPPROVED.
The test key is a synthetic, externally supplied *scoped* key. There is no
K_root derivation, enrollment, Gateway grant, persistent state, or RF path here.
Objects are synthetic frame-shaped bytes, not valid AEAD ciphertext.
"""

import hashlib
import hmac
import unittest
from unittest.mock import patch

DOMAIN = b"ORUN-TLP-V2-CUSTODY-BATCH-ACK-MAC-v1"
KEY = bytes(range(32))  # TEST ONLY; never provision
MAX_ENTRIES = 4
ENTRY_SIZE = 9
PREFIX = 32
TAG_SIZE = 16


def digest(obj):
    return hashlib.sha256(obj).digest()


def ref(obj):
    return int.from_bytes(digest(obj)[:8], "big")


def frame(n, seed):
    assert n in (84, 100)
    out = bytearray(((i * 17 + seed) & 255) for i in range(n))
    out[0:8] = bytes((2, 4, 2, 1 if n == 100 else 2, 3, 1,
                       64 if n == 100 else 48, 0))
    out[20:28] = (seed + 1).to_bytes(8, "big")
    return bytes(out)


def valid_shape(obj):
    if len(obj) not in (84, 100):
        return False
    return (obj[:3] == bytes((2, 4, 2)) and
            obj[3] == (1 if len(obj) == 100 else 2) and
            obj[4] in (2, 3) and obj[5] == 1 and
            obj[6] == (64 if len(obj) == 100 else 48) and
            obj[7] == 0 and any(obj[20:28]) and
            obj[16:20] != b"\xff" * 4)


def encode_unsigned(objects, tracker=0x0102030405060708,
                    gateway=0x1112131415161718, epoch=0x12345678,
                    floor=0x90ABCDEF, grant=0x10203040, flags=1):
    if not 1 <= len(objects) <= MAX_ENTRIES or flags not in (0, 1):
        raise ValueError("count or flags")
    if not all(valid_shape(o) for o in objects):
        raise ValueError("object shape")
    objs = sorted(objects, key=lambda o: (len(o), ref(o)))
    entries = [(len(obj), ref(obj)) for obj in objs]
    if len(set(entries)) != len(entries):
        raise ValueError("duplicate object")
    head = (bytes((2, 6, flags, len(objs))) +
            tracker.to_bytes(8, "big") + gateway.to_bytes(8, "big") +
            epoch.to_bytes(4, "big") + floor.to_bytes(4, "big") +
            grant.to_bytes(4, "big"))
    for length, hint in entries:
        head += bytes((length,)) + hint.to_bytes(8, "big")
    return head, objs


def test_only_sign(objects, key=KEY, **kwargs):
    head, objs = encode_unsigned(objects, **kwargs)
    mac = hmac.new(key, DOMAIN + head + b"".join(map(digest, objs)),
                   hashlib.sha256).digest()[:TAG_SIZE]
    return head + mac


def preflight_untrusted_host_fixture(wire, cached, key, trusted):
    """Test-only preflight. Returns selected cached indices, NEVER release auth.

    The 'trusted' dictionary is a manually set TEST FIXTURE, not actual
    credential- or enrollment-authority state; do not wire into production.
    """
    if not isinstance(wire, bytes) or not isinstance(key, bytes) or len(key) != 32:
        return None
    if len(wire) < 57 or len(wire) > 84 or wire[0:2] != b"\x02\x06":
        return None
    if wire[2] & ~1:
        return None
    n = wire[3]
    if not 1 <= n <= MAX_ENTRIES or len(wire) != PREFIX + n * ENTRY_SIZE + TAG_SIZE:
        return None
    if not trusted.get("active", False):
        return None
    ids = (int.from_bytes(wire[4:12], "big"),
           int.from_bytes(wire[12:20], "big"),
           int.from_bytes(wire[20:24], "big"),
           int.from_bytes(wire[24:28], "big"),
           int.from_bytes(wire[28:32], "big"))
    if ids != trusted.get("scope"):
        return None
    entries = []
    for i in range(n):
        at = PREFIX + i * ENTRY_SIZE
        entry = (wire[at], int.from_bytes(wire[at + 1:at + 9], "big"))
        if entry[0] not in (84, 100) or (entries and entry <= entries[-1]):
            return None
        entries.append(entry)
    if not 1 <= len(cached) <= MAX_ENTRIES or not all(valid_shape(o) for o in cached):
        return None
    chosen = []
    full = []
    # All entries must find exactly one immutable locally retained ciphertext.
    for entry in entries:
        matches = [i for i, obj in enumerate(cached)
                   if (len(obj), ref(obj)) == entry]
        if len(matches) != 1 or matches[0] in chosen:
            return None
        chosen.append(matches[0])
        full.append(digest(cached[matches[0]]))
    candidate = hmac.new(key, DOMAIN + wire[:-TAG_SIZE] + b"".join(full),
                         hashlib.sha256).digest()[:TAG_SIZE]
    if not hmac.compare_digest(candidate, wire[-TAG_SIZE:]):
        return None
    return tuple(chosen)


class BatchMacReferenceTests(unittest.TestCase):
    def setUp(self):
        self.cache = [frame(100, 1), frame(84, 2), frame(100, 3), frame(84, 4)]
        self.scope = (0x0102030405060708, 0x1112131415161718,
                      0x12345678, 0x90ABCDEF, 0x10203040)
        self.trusted = {"active": True, "scope": self.scope}

    def verify(self, wire, cache=None, key=KEY, trusted=None):
        return preflight_untrusted_host_fixture(
            wire, self.cache if cache is None else cache, key,
            self.trusted if trusted is None else trusted)

    def test_all_counts_and_exact_selected(self):
        for n in (1, 2, 3, 4):
            wire = test_only_sign(self.cache[:n])
            self.assertEqual(len(wire), 48 + 9 * n)
            selected = self.verify(wire)
            self.assertEqual(set(selected), set(range(n)))
            # A partial ACK explicitly does not claim older/newer unlisted data.

    def test_fixed_independent_digest_and_tag_vector(self):
        obj = self.cache[0]
        self.assertEqual(digest(obj).hex(), "675e405646fd2800fd91958aee1258cf3ed85e74d4a008f4ea487ba5a8c9a6e9")
        wire = test_only_sign([obj])
        self.assertEqual(wire.hex(), "02060101010203040506070811121314151617181234567890abcdef1020304064675e405646fd28002115db378975fc5a905995439269eb2f")
        self.assertEqual(self.verify(wire), (0,))

    def test_mutation_missing_duplicate_reordered_and_wrong_scope(self):
        wire = test_only_sign(self.cache)
        self.assertIsNotNone(self.verify(wire))
        for index in (0, 1, 2, 3, 4, 11, 12, 19, 20, 23, 24, 27,
                      28, 31, 32, 35, len(wire) - 1):
            mutated = bytearray(wire)
            mutated[index] ^= 0x01
            self.assertIsNone(self.verify(bytes(mutated)), index)
        self.assertIsNone(self.verify(wire[:-1]))
        self.assertIsNone(self.verify(wire + b"\0"))
        self.assertIsNone(self.verify(wire, self.cache[:2]))
        self.assertIsNone(self.verify(wire, self.cache + [self.cache[0]]))
        self.assertIsNone(self.verify(wire, self.cache, bytes(reversed(KEY))))
        self.assertIsNone(self.verify(wire, trusted={"active": False, "scope": self.scope}))
        for i in range(5):
            scope = list(self.scope)
            scope[i] += 1
            self.assertIsNone(self.verify(wire, trusted={"active": True, "scope": tuple(scope)}))
        # Packet bytes are still structurally parsable after a MAC mutation;
        # only the authenticated preflight rejects them.
        tampered = bytearray(wire)
        tampered[-1] ^= 1
        self.assertIsNone(self.verify(bytes(tampered)))

    def test_partial_and_old_records_cannot_be_cumulatively_released(self):
        wire = test_only_sign([self.cache[0], self.cache[2]])
        self.assertEqual(set(self.verify(wire)), {0, 2})
        self.assertIsNone(self.verify(wire, cache=[self.cache[1], self.cache[3]]))
        # Duplicate and stale exact-byte references are refused rather than
        # treating a claimed latest record as a prefix-delivery watermark.
        self.assertIsNone(self.verify(wire, cache=[self.cache[0], self.cache[0],
                                                    self.cache[2]]))

    def test_deliberate_64_bit_hint_collision_is_ambiguous(self):
        # A short hint is NEVER a unique identity proof. Model a hypothetical
        # collision; the complete 32B digests remain distinct for the MAC.
        wire = test_only_sign([self.cache[0]])
        base_digest = digest(self.cache[0])
        other_digest = digest(self.cache[2])
        forged_collision_digest = base_digest[:8] + other_digest[8:]
        assert forged_collision_digest != base_digest
        original_digest = digest

        def collision_hash(obj):
            if obj == self.cache[2]:
                return forged_collision_digest
            return original_digest(obj)

        with patch.dict(globals(), {"digest": collision_hash}):
            self.assertIsNone(self.verify(wire))

    def test_malformed_or_replayed_different_context(self):
        wire = test_only_sign([self.cache[0]])
        for payload in (b"", b"\x02\x05" + wire[2:],
                        b"\x02\x03" + wire[2:], b"\x01\x06" + wire[2:]):
            self.assertIsNone(self.verify(payload))
        different_gateway = test_only_sign([self.cache[0]],
                                          gateway=self.scope[1] + 1)
        self.assertIsNone(self.verify(different_gateway))
        self.assertIsNone(self.verify(wire, key=b"short"))
        changed = bytearray(self.cache[0]); changed[-1] ^= 1
        self.assertIsNone(self.verify(wire, cache=[bytes(changed)]))
        with self.assertRaises(ValueError):
            test_only_sign([self.cache[0]] * 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
