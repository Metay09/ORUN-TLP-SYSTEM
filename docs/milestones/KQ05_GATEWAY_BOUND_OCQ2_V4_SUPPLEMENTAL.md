# KQ05 — Gateway OCQ2/v4 28 KiB device-bound storage (HOST)

**Status:** Draft, pending PC verification. NOT mounted by product firmware.
Depends on previously PC-PASSed KQ05 role/legacy gate PR #342.

## Safety issue addressed

OCQ2/v3 page headers in the existing Gateway `ProductCustodyStore`
have no durable Gateway device ID. The prospective 39-page 128 KiB +
28 KiB store must NOT silently adopt unrelated or stale OCQ2/v3 bytes
in the old History Flash partition.

## Solution with no capacity lost

The 64-byte OCQ2 page header already reserves bytes 40..63 as erased.
The **new OCQ2/v4 supplemental pages only** use exactly these 24 bytes:

- 40..47: local physical Gateway ID (big-endian uint64).
- 48..55: bitwise complement of Gateway ID.
- 56..59: CRC32 over immutable bytes 0..31 and 40..55.
- 60..63: fixed `GW4B` binding marker.

The original header version (v3) stays valid in primary pages
0..31. These 32 pages remain the same physical 128 KiB storage.
Supplemental logical pages **32..38** must use the new v4 header,
and `ProductCustodyStore` requires a local non-zero/non-FF Gateway
identity in 39-page mode. A v3, wrong-ID, invalid-CRC, mixed-format,
foreign or torn header in the 28 KiB region **fails closed**.

The new page state machine uses an additional **24-byte NOR program**
after header body write and before the existing separate commit and
activation marker writes. A power cut prior to commit leaves a staged
page which is **not** accepted as a durable custody owner. The page
format does not change existing 33 objects per page, including every
one of the seven supplemental pages (**231 nominal extra slots**).
Physical usable capacity is subject to wear/reclaim/commit overhead.

At boot, the existing complete 28 KiB **read-only** census now
distinguishes `kGatewayBoundCandidate` (v4 local ID only) from
`kGatewayNeedsBinding` (legacy v3). Mixed v3/v4 pages are refused.
The test-only segmented bridge only opens Gateway supplemental pages
if all non-erased headers are v4 bound to its local ID. The full
`ProductCustodyStore::begin()` still scans/recovers all pages and
records before admitting a write.

**Security distinction:** This is **non-cryptographic local Flash
ownership binding** (ID + complement + CRC), not proof of a signed
Gateway identity, source authentication, authorization, MAC or custody
ACK. A caller-provided ID, by itself, is NOT permission to mint ACK.
Future production composition must obtain the ID from trusted board
identity and verified role/grants, not packet fields. This branch
implements no wire protocol or signing logic.

## PC regression

`bash tools/ci/run_kq05_gateway_bound_ocq2_host.sh`

The gate checks OCQ2/v3 compatibility, v4 parsing, invalid/mutated ID
or complement/CRC/marker, crash-staged header, foreign Gateway ID,
missing ID, unbound old supplemental v3, mixed formats, real
`ProductCustodyStore` page-32 spill with v4 binding, reboot recovery,
duplicate suppression, and no writes to primary on spill.

The existing role-aware 28 KiB census and 39-page Gateway host tests
are rerun by this command.

## NOT DONE

- **No device firmware currently calls a 39-page ProductCustodyStore**,
  and no physical device starts a writer in the old History region.
  Product device runtime stays on the original 32-page store.
- No operator-authorized legacy History migration/erase. No old records
  may be silently deleted or overwritten; 28 KiB must first pass
  independent liability and migration policy.
- No signed/granted Gateway identity or cryptographic proof at the
  39-page mount boundary; current host adapter's `bound_` and
  `exclusive_` arguments are simulated, not real evidence.
- No end-to-end Gateway selective *batch custody ACK*, multi-Gateway
  deduplication or Tracker HMAC-based release. Backend receipts are a
  separate proof, not substitutes for Gateway custody.
- No physical power-loss, Nordic firmware build/RAM, radio budget or
  release qualification. Issue #221 field gate stays BLOCKED.
