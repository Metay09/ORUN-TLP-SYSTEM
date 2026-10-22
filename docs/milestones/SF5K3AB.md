# SF5K3AB — signed Gateway enrollment append-only preflight

**Base:** `182eee2b71b4069b1539749c65756cbe6f674609` (SF5K3AA).

## Why this bounded slice?

We have a read-only, issuer-authenticated OGK1 snapshot reader (SF5K3Y),
real host Ed25519 proof + independent revision gate (SF5K3Z), and a
standalone RAK4630 CC310 signature-verification test image (SF5K3AA).
We do NOT yet have a physical monotonic trusted revision checkpoint or a
secure provisioning writer. Adding one hastily risks rollback, grant/key
substitution and the already-provisioned Tracker's retained records.

## What is implemented

`GatewaySignedGrantAppendPreflight::inspect()` compares two independently
issuer-verified `GatewayGrantSnapshotReader` objects: the locally enrolled
source and an off-device signed proposal. Both readers are separately
bound to the same exact nonzero Gateway hardware identity.

Only a **strict grant append** is eligible for subsequent review:

- Existing 56-byte Tracker grant slots are byte-identical in the proposal:
  identity, key epoch, custody floor, grant generation, flags and full
  32-byte scoped ACK key are preserved.
- Previously empty slots may gain new independently signed grants, subject
  to OGK1's existing duplicate-ID and format validation. At least one
  grant MUST be newly added. There is no reordering or hidden mutation.
- The proposed signed snapshot generation is exactly active + 1,
  without overflow, and the active source equals the trusted *independent*
  revision anchor. The anchor and both readers are checked again after
  comparison to reject in-loop revocation/authority changes.
- The result is an **informational verdict**:
  `kEligibleForSeparateCommitReview`. It is NOT permission to write flash,
  advance the checkpoint, install a key, authenticate a radio message or
  send a custody ACK. A future writer must revalidate the complete exact
  signed proposal under exclusive ownership immediately before mutation.

Deliberately not supported: deleting/revoking a Tracker, rotating its key,
altering its epoch/floor/generation, moving slots, initial enrollment
without an independent protected anchor, writing/erasing either security
page, and reconnecting a gateway whose revision provenance is uncertain.
These all need separate reviewed operations and durable rollback semantics.
A normal `ORS1` Tracker SecurityStore may NEVER be mounted in parallel
with this Gateway-specific security partition.

## Verification requested

```sh
bash tools/ci/run_sf5k3ab_signed_append_preflight_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
pio run -d firmware -e rak4630_sf5k3aa_ed25519_probe
```

The focused host tests use real OpenSSL Ed25519 signatures, the actual
immutable OGK1 reader, and a FAKE independent revision anchor; checks
include append-only happy path, unchanged/removed/changed existing keys,
signed wrong-version proposals, changed CRC without fresh signature,
wrong issuer and wrong Gateway, and RAM grant revocation. The fake anchor
is not production authorization. No device upload, storage erase or live
Gateway activation takes place.

## Remaining hard blocks

The production trusted Ed25519 issuer public-key delivery/pinning,
independent physical rollback-resistant revision anchor, secure initial
provisioning and A/B commit-last writer, power-loss qualification, positive
physical Ed25519 execution, true Gateway product-region ownership,
origin-spoof/storage exhaustion defenses, relay ACK path and real RF E2E
qualification remain incomplete. SF5K3AB removes none of those blockers.
