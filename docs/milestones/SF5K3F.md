# SF5K3F / 1 — production v3 format and journal validation gate

**Status: PRODUCT-SOURCE CODEC ONLY; HOST TESTS NOT YET EXECUTED.**

The previous SF5K3E has host and RAK4630 compilation PASS. Its
`SecurityStore::inspectV3PageReadOnly` can inspect v3 but
`SecurityStore::begin/recover` still refuses v3 as active authority,
and the writer continues to emit **v2**.

This PR is deliberately stacked on SF5K3E and establishes the
production-source byte contract required before changing an A/B writer.
It introduces:

- `firmware/include/security_v3_format.h`
- `firmware/src/security_v3_format.cpp`
- `firmware/tests/sf5/test_sf5k3f_security_v3_product_format.cpp`

The codec implements byte-exact, fully checked 56-byte typed records;
the mandatory five-entry committed policy snapshot (one meta record and
all four Gateway entries, including empty slots and revocation tombstones);
global Gateway generation monotonicity, floor/revision scope, 256/8-block
TX and A2D exclusive reservations; erased-tail and gap detection, burned
pre-commit entries, and rejection of partial/unknown commit state. Every
decoder publishes output only after the whole requested record/journal
has passed integrity and semantic checks.

The first test series compares product-source records and the exact
five-entry prefix **byte for byte** against the already-validated
SF5K3C/D independent host-model implementations, then replays their
full 4 KiB v3 page fixtures. It exercises every prefix byte corruption
and every append byte cut, trailing CRC-correct but semantically invalid
records, duplicate Gateway slots, missing prefix and dirty reserved tail.
No real Gateway ID or root secret is printed.

**Not in this PR**: activating the v3 version in
`security_format::kVersion`, v3 active SecurityStore recovery,
flash writes, SoftDevice event ownership, migration from v1/v2, live
Gateway grants, authentication/HKDF/HMAC admission, RF ACK, record release.
The codec is pure calculation and may not create security authority on
its own. Old binaries must still fail closed on an activated v3 page.

## Why v3 live writing remains blocked

Today `SecurityStore::startNewPage` copies only TX/A2D bounds and
credential to a replacement v2 page. Switching a header version to 3
would discard revoked Gateway generations, the global policy floor
and exact policy revision on compaction. A valid v3 writer must instead:

1. Consume **only** a fully recovered authoritative store state,
   not an ACK, public setter, or raw encoded record.
2. Retain the credential/root, both exclusive bounds, exact policy
   revision and floor, and all four grant generations/tombstones when
   snapshotting an existing credential.
3. For a new credential with independently new ID/root, reset **all**
   grants, policy and counter lifetime; follow the existing credential
   commit decision contract (same numerical key epoch can be legal).
4. Program/verify 52 record bytes; program/verify 4-byte COMMIT last;
   fully verify all snapshot+counter entries; write/verify the page
   header's activation COMMIT **last**. Never activate a partial page.
5. Select between two committed A/B pages only after comparing full
   same-credential root, exclusive-bound and policy continuity; uncertain
   flash completion must freeze protected service until reboot.
6. Run power-cut, corrupt-readback, nonce reservation, v1/v2 migration,
   credential rotation and flash-wear tests through the **real**
   `SecurityStore` + `FlashMutationGate` code, then RAK4630 build and
   hardware qualification. Never emit an ACK or release stored records
   until separate authenticated custody provisioning and durable receipt
   boundaries are complete.

Tests are connected to both
`bash tools/ci/run_sf5k3a_host.sh` and
`bash firmware/tests/run_host_tests.sh`.
To validate the current product format source, run both and
`cd firmware && pio run -e rak4630` on this feature branch.
**No PASS is claimed until those commands execute.**
