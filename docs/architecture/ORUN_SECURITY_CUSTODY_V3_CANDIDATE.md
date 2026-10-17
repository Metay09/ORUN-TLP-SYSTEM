# ORUN SecurityStore-v3 custody-policy journal — SF5K3C candidate

**STATUS: HOST-ONLY SECURITY FORMAT CANDIDATE, NOT APPROVED FOR PRODUCT FLASH.**
The production SecurityStore continues to use v2. The files in
`firmware/tests/sf5/sf5k3c_security_v3_candidate.{h,cpp}` are intentionally
outside `firmware/src`; they neither change runtime enrollment nor enable
custody ACK acceptance or record release.

## Why a new version is necessary

The present 2-page SecurityStore stores a 32-byte ORS1 header, a 68-byte
credential and up to 99 40-byte v2 typed state records. Its only valid kinds
are TX reserve and authenticated A2D replay reserve. The remaining 36 bytes
are a **reserved/erased integrity boundary**, not unused grant storage.
Overlaying a Gateway ID, custody floor or grant generation on a v2 kind or
on the tail would change rollback/recovery semantics and is forbidden.

SF5B section 12.4 requires a **separate custody capability**, unrelated to
COMMAND delegated slots and independently revocable. Its current proposed
bounds are one tracker, one current credential epoch, one custody floor and
at most four enrolled Gateway IDs with per-slot grant generation. A mobile
Gateway uses the same role/capability; a transport-only Relay gets none.

## Proposed v3 physical geometry (not activated)

Keep the same *two physical 4096-byte pages* and the ORS1 A/B commit-last
selection semantics. Use an explicit header version 3; an old v2 firmware
MUST fail closed on a committed unknown higher version.

| Region in each page | Bytes | Rule |
|---|---:|---|
| ORS1 version-3 header | 32 | device, generation, CRC, activation commit last |
| Credential record | 68 | unchanged current credential bytes, key epoch and CRC |
| 71 uniform v3 journal entries | 71 × 56 = 3976 | append-only, CRC and commit last |
| Reserved tail | 20 | all 0xFF or fail recovery |
| **Total** | **4096** | No extra partition |

A proposed 56-byte state record (big-endian) is:

| Offset | Bytes | Purpose |
|---|---:|---|
| 0 | 16 | credential ID |
| 16 | 4 | key epoch |
| 20 | 1 | kind: 1 TX bound, 2 A2D bound, 3 custody policy |
| 21 | 1 | Gateway slot (0..3) or 0xFF for global/counter record |
| 22 | 2 | reserved zero |
| 24 | 8 | counter exclusive bound, or Gateway ID (zero for revocation) |
| 32 | 4 | per-slot grant generation (zero only for counters/floor-only; a revocation advances it) |
| 36 | 4 | custody policy floor (zero for counters) |
| 40 | 4 | policy revision (zero for counters) |
| 44 | 4 | reserved zero |
| 48 | 4 | CRC32 of bytes 0..47 |
| 52 | 4 | commit marker zero, written last |

v3 counter entries still require 256-aligned TX reserve or 8-aligned A2D
replay exclusive bounds, increasing monotonically exactly as v2. These
state records are longer, reducing append slots from 99 to 71 and requiring
a **real wear/compaction qualification** before format approval.

## Grant-transition invariants

- A change is a **single atomic authenticated policy entry**, never four
  separately committed slot writes.
- Every authenticated policy entry advances `policy_revision` by exactly 1.
  A grant/revoke/replace for one Gateway advances only **that slot's**
  `grant_generation` monotonically. **All slot updates additionally require
  a generation strictly above the maximum generation across all four slots,**
  because the ACK binds Gateway ID and generation but not physical slot number.
  This is a compact global high-water rule, not a global ACK key. Revocation
  persists a *zero Gateway ID* with its new nonzero generation as a tombstone,
  preventing old ACK reuse even if that Gateway moves to a different slot
  after re-enrollment. Another Gateway slot is not implicitly revoked.
  Exhausting the non-max 32-bit generation space fails closed until a
  separately reviewed rekey/rebaseline.
- `custody_floor` may be zero (only `UINT32_MAX` is invalid under SF5B).
  It is nondecreasing. An explicit floor-only policy update must increase it,
  invalidating all earlier ACK floor/key scopes. Ordinary per-slot updates
  can retain the same floor, so independent Gateways need not rekey.
- Each active Gateway ID must be unique among four slots and have a
  nonzero/non-max slot generation. Revocation is not a COMMAND revoke.
- Different credential ID/key epoch, stale revision, rollback of floor,
  rollback of TX/A2D bounds, a duplicate Gateway, or unknown kind all
  **fail closed**. An ACK cannot mutate this state.
- At boot, an unprovisioned custody policy authorizes **zero Gateways**.
  Any v1/v2 migration must retain nonce/replay exclusive bounds while
  starting custody policy unprovisioned.
- A lower-version firmware encountering a **committed** v3 page must stop
  in unsupported state, not fall back to an older v2 authority page.
- A never-provisioned custody policy has four empty slots regardless of
  whether its current policy floor is zero. The ACK key is still bound to
  the real root credential and per-Gateway grant scope, not this host model.

## Power-cut behavior modeled by SF5K3C

For one 56-byte entry: program/verify the first 52 bytes, write/verify the
4-byte zero COMMIT last. On restart, a non-erased entry with erased COMMIT
burns its slot; a partially programmed COMMIT or committed invalid CRC
faults closed. A fully erased slot followed by non-erased entries is a hole
and fails recovery. Return reconstructed state **only if the entire journal
is valid**, never a partially accepted grant set.

The host candidate and its tests model byte cuts, floor/revision rollback,
retained TX/A2D bounds, rejected malformed records, page v2-v3 header
distinction, cross-slot Gateway re-enrollment with stale generations, and
the four-slot policy. These are NOT evidence that the
real `SecurityStore` can yet migrate, append, compact, reconcile uncertain
flash completion, or authenticate enrollment.

## Gates before production integration

1. Independent security/flash review of the v3 geometry, wear effects,
   policy floor semantics, revocation and the absence of older-firmware
   downgrade escape routes. Reconcile header/policy compaction semantics
   across v3 A/B generations without silently refreshing floor on copy.
2. Implement v1/v2 recovery and **power-cut safe v3 migration** in the
   single real SecurityStore, including snapshots during compaction and
   updated fixed buffer limits and physical backend readback. Test old
   committed page + staged newer page, reset at every erase/body/commit/
   activation point, and credential rotation.
3. Authenticated *policy update transport* and independent non-exportable
   scoped custody key derivation/verification. No directly writable
   `grantScopeMatches` or public remote grant setter.
4. Verify on the actual RAK4630 CC310 provider with independent HKDF/HMAC
   test vectors; only then connect the 0x05 exact-object ACK verifier to
   `ProductRecordStore`'s exclusive durable release owner.
5. Physical ACK, Relay reverse path, lost ACK, deep-sleep/power cut,
   Gateway local durable custody or signed Edge acceptance, and battery/
   regulatory airtime qualification.

**Until all gates pass:** no production Gateway custody ACK is accepted and
no retained Tracker observation is released on the basis of this proposal.
