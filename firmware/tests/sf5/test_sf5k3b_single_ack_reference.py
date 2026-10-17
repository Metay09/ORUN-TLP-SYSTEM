#!/usr/bin/env python3
"""SF5K3B 0x05 single-object custody ACK host-only reference.

Implements draft SF5B section 12 SHA256/HKDF/HMAC byte contracts with synthetic
ciphertext-shaped fixtures. NOT a product verifier: its credential, enrollment,
and key fixtures are NOT SecurityStore authority; no RF or release function.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import hmac
import unittest
from unittest.mock import patch

KDF_INFO = b"ORUN-TLP-V2-GW-CUSTODY-ACK-v1"
MAC_DOMAIN = b"ORUN-TLP-V2-CUSTODY-ACK-MAC-v1"
KEY_ROOT = bytes(range(32))  # SYNTHETIC TEST ONLY, do not provision
CREDENTIAL_ID = bytes(range(16))  # SYNTHETIC TEST ONLY
HEADER_SIZE = 40
WIRE_SIZE = 56
MAX_CACHE = 4


@dataclass(frozen=True)
class FixtureGrant:
    # Test fixture values only. A real grant must be enrolled, independently
    # authenticated and DURABLY persisted under the SecurityStore owner.
    tracker: int = 0x0102030405060708
    gateway: int = 0x1112131415161718
    epoch: int = 0x12345678
    floor: int = 0x90ABCDEF
    generation: int = 0x10203040
    active: bool = True
    credential_id: bytes = CREDENTIAL_ID
    root: bytes = KEY_ROOT


def scope_valid(g: FixtureGrant) -> bool:
    return (g.active and 0 < g.tracker <= 0xFFFFFFFFFFFFFFFF
            and 0 < g.gateway <= 0xFFFFFFFFFFFFFFFF
            and 0 <= g.epoch < 0xFFFFFFFF
            and 0 <= g.floor < 0xFFFFFFFF
            and 0 < g.generation < 0xFFFFFFFF
            and isinstance(g.credential_id, bytes)
            and len(g.credential_id) == 16
            and isinstance(g.root, bytes) and len(g.root) == 32)


def ack_key(g: FixtureGrant) -> bytes:
    # HKDF-Extract(salt=credential_id, IKM=K_root).
    # HKDF-Expand(info=ASCII label + exact tuple, output=32B).
    if not scope_valid(g):
        raise ValueError("inactive or invalid fixture grant")
    info = (KDF_INFO + g.tracker.to_bytes(8, "big")
            + g.epoch.to_bytes(4, "big")
            + g.gateway.to_bytes(8, "big")
            + g.floor.to_bytes(4, "big")
            + g.generation.to_bytes(4, "big"))
    prk = hmac.new(g.credential_id, g.root, hashlib.sha256).digest()
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()


def digest(frame: bytes) -> bytes:
    return hashlib.sha256(frame).digest()


def synthetic_protected_frame(size: int, seed: int,
                              grant: FixtureGrant = FixtureGrant()) -> bytes:
    # Frame is syntactically plausible but NOT valid AES-CCM ciphertext/tag.
    # That distinction is deliberate; no simulated evidence is custody proof.
    if size not in (84, 100) or seed <= 0:
        raise ValueError("test fixture size/seed")
    data = bytearray((seed + i * 19) & 0xFF for i in range(size))
    data[:8] = bytes((2, 4, 2, 1 if size == 100 else 2, 3, 1,
                      64 if size == 100 else 48, 0))
    data[8:16] = grant.tracker.to_bytes(8, "big")
    data[16:20] = grant.epoch.to_bytes(4, "big")
    data[20:28] = seed.to_bytes(8, "big")
    return bytes(data)


def shape_matches(frame: bytes, g: FixtureGrant) -> bool:
    if not isinstance(frame, bytes) or len(frame) not in (84, 100):
        return False
    return (frame[:3] == b"\x02\x04\x02"
            and frame[3] == (1 if len(frame) == 100 else 2)
            and frame[4] == 3 and frame[5] == 1
            and frame[6] == (64 if len(frame) == 100 else 48)
            and frame[7] == 0
            and frame[8:16] == g.tracker.to_bytes(8, "big")
            and frame[16:20] == g.epoch.to_bytes(4, "big")
            and any(frame[20:28]))


def sign_fixture(frame: bytes, g: FixtureGrant,
                 *, relay_allowed: bool = True) -> bytes:
    if not shape_matches(frame, g):
        raise ValueError("invalid synthetic protected frame")
    d = digest(frame)
    head = (bytes((2, 5, 1 if relay_allowed else 0, len(frame)))
            + g.tracker.to_bytes(8, "big")
            + g.gateway.to_bytes(8, "big")
            + g.epoch.to_bytes(4, "big")
            + g.floor.to_bytes(4, "big")
            + g.generation.to_bytes(4, "big")
            + d[:8])
    assert len(head) == HEADER_SIZE
    mac = hmac.new(ack_key(g), MAC_DOMAIN + head + d,
                   hashlib.sha256).digest()[:16]
    return head + mac


def preflight_test_only(wire: bytes, cached: tuple[bytes, ...],
                        trusted: FixtureGrant) -> int | None:
    """Return unique cached index for TEST assertion, NEVER release authority.

    No way to get authority from a wire-field claim or synthetic grant:
    production must first implement durable Gateway custody enrollment/grant,
    an isolated secret-bearing HMAC checker and one mutation owner.
    """
    if not isinstance(wire, bytes) or len(wire) != WIRE_SIZE:
        return None
    if not isinstance(trusted, FixtureGrant) or not scope_valid(trusted):
        return None
    if wire[:2] != b"\x02\x05" or wire[2] not in (0, 1):
        return None
    if wire[3] not in (84, 100):
        return None
    expected_scope = (trusted.tracker.to_bytes(8, "big")
                      + trusted.gateway.to_bytes(8, "big")
                      + trusted.epoch.to_bytes(4, "big")
                      + trusted.floor.to_bytes(4, "big")
                      + trusted.generation.to_bytes(4, "big"))
    if not hmac.compare_digest(wire[4:32], expected_scope):
        return None
    if not isinstance(cached, tuple) or not (1 <= len(cached) <= MAX_CACHE):
        return None
    # Snapshot must come from a store with NO pending flash mutation. This
    # reference does not and cannot manufacture such a durable-state fact.
    if any(not shape_matches(obj, trusted) for obj in cached):
        return None
    if len(set(cached)) != len(cached):
        return None
    matches = []
    for i, obj in enumerate(cached):
        d = digest(obj)
        if len(obj) == wire[3] and hmac.compare_digest(d[:8], wire[32:40]):
            matches.append((i, d))
    # Colliding 64-bit hints are NEVER treated as sufficient identity.
    if len(matches) != 1:
        return None
    idx, full_digest = matches[0]
    tag = hmac.new(ack_key(trusted),
                   MAC_DOMAIN + wire[:HEADER_SIZE] + full_digest,
                   hashlib.sha256).digest()[:16]
    return idx if hmac.compare_digest(tag, wire[HEADER_SIZE:]) else None


class SingleCustodyAckHostTests(unittest.TestCase):
    def setUp(self):
        self.g = FixtureGrant()
        self.frames = (synthetic_protected_frame(100, 1, self.g),
                       synthetic_protected_frame(84, 2, self.g),
                       synthetic_protected_frame(100, 3, self.g),
                       synthetic_protected_frame(84, 4, self.g))

    def test_hkdf_scoped_key_fixed_vector(self):
        self.assertEqual(len(ack_key(self.g)), 32)
        self.assertEqual(ack_key(self.g).hex(),
                         "512f2bc3bd052e5de60deecd66e006a66956b5288facf397128bade3860faf06")
        self.assertNotEqual(ack_key(self.g), ack_key(replace(
            self.g, generation=self.g.generation + 1)))
        self.assertNotEqual(ack_key(self.g), ack_key(replace(
            self.g, gateway=self.g.gateway + 1)))
        self.assertNotEqual(ack_key(self.g), ack_key(replace(
            self.g, epoch=self.g.epoch + 1)))

    def test_each_frame_and_exact_bytes(self):
        for i, frame in enumerate(self.frames):
            wire = sign_fixture(frame, self.g)
            self.assertEqual(len(wire), 56)
            self.assertEqual(wire[:4], bytes((2, 5, 1, len(frame))))
            self.assertEqual(wire[32:40], digest(frame)[:8])
            self.assertEqual(preflight_test_only(wire, self.frames, self.g), i)
            self.assertEqual(preflight_test_only(wire, self.frames, self.g), i)
            self.assertIsNone(preflight_test_only(wire, tuple(
                f for j, f in enumerate(self.frames) if j != i), self.g))
        self.assertEqual(sign_fixture(self.frames[0], self.g).hex(),
                         "02050164010203040506070811121314151617181234567890abcdef102030408409db542c57ae253f6b8583c61901c56f71c17d636283c2")

    def test_wrong_grant_and_credential_never_pass(self):
        wire = sign_fixture(self.frames[0], self.g)
        choices = (replace(self.g, active=False),
                   replace(self.g, tracker=self.g.tracker + 1),
                   replace(self.g, gateway=self.g.gateway + 1),
                   replace(self.g, epoch=self.g.epoch + 1),
                   replace(self.g, floor=self.g.floor + 1),
                   replace(self.g, generation=self.g.generation + 1),
                   replace(self.g, credential_id=bytes(reversed(CREDENTIAL_ID))),
                   replace(self.g, root=bytes(reversed(KEY_ROOT))),
                   replace(self.g, generation=0),
                   replace(self.g, floor=0xFFFFFFFF))
        for g in choices:
            self.assertIsNone(preflight_test_only(wire, self.frames, g))
        forged = sign_fixture(self.frames[0], replace(
            self.g, gateway=self.g.gateway + 1))
        self.assertIsNone(preflight_test_only(forged, self.frames, self.g))

    def test_malformed_or_tampered_wire(self):
        good = sign_fixture(self.frames[0], self.g)
        for index in range(WIRE_SIZE):
            altered = bytearray(good)
            altered[index] ^= 1
            self.assertIsNone(preflight_test_only(
                bytes(altered), self.frames, self.g), index)
        for wire in (b"", good[:-1], good + b"x", b"\x02\x06" + good[2:]):
            self.assertIsNone(preflight_test_only(wire, self.frames, self.g))
        other_path = sign_fixture(self.frames[0], self.g, relay_allowed=False)
        self.assertEqual(preflight_test_only(other_path, self.frames, self.g), 0)

    def test_no_implicit_prefix_and_missing_or_released(self):
        wire = sign_fixture(self.frames[2], self.g)
        self.assertEqual(preflight_test_only(wire, self.frames, self.g), 2)
        self.assertIsNone(preflight_test_only(wire, self.frames[:2], self.g))
        # Once flash release makes an exact object invisible to the snapshot,
        # its old valid ACK cannot be replayed as a new release authority.
        still_retained = (self.frames[0], self.frames[1], self.frames[3])
        self.assertIsNone(preflight_test_only(wire, still_retained, self.g))
        self.assertIsNone(preflight_test_only(wire, tuple(), self.g))
        self.assertIsNone(preflight_test_only(wire, self.frames + self.frames[:1],
                                              self.g))
        self.assertIsNone(preflight_test_only(
            wire, (self.frames[2], self.frames[2]), self.g))

    def test_full_digest_and_short_ref_collision(self):
        good = sign_fixture(self.frames[0], self.g)
        authentic_digest = digest(self.frames[0])
        different = digest(self.frames[2])
        injected_digest = authentic_digest[:8] + different[8:]
        self.assertNotEqual(injected_digest, authentic_digest)
        true_digest = digest

        def collide(obj):
            if obj == self.frames[2]:
                return injected_digest
            return true_digest(obj)

        with patch.dict(globals(), {"digest": collide}):
            self.assertIsNone(preflight_test_only(good, self.frames, self.g))
        tampered = bytearray(self.frames[0])
        tampered[-1] ^= 1
        self.assertIsNone(preflight_test_only(
            good, (bytes(tampered),), self.g))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SingleCustodyAckHostTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    print("SF5K3B 0x05 single-object host HMAC/HKDF scope oracle: PASS")
