# SF5K3J — Real SecurityStore v3 Gateway policy journal (private admission)

**Status:** Draft integration, no new target-device firmware upload, NOT
a radio/BLE authorization or Gateway ACK acceptance feature.
**Validation:** SF5K3J local host compile/run and target RAK4630 build
are PENDING. The previously supplied SF5K3I integration PASS does not
qualify the new policy writer.

## Functional code

The actual `SecurityStore` now has a **private** Gateway policy
mutation state machine, persisted on its real production-source
`FlashBackend` asynchronous flash mutation path. It uses 56-byte
`security_v3_format::Kind::kCustodyPolicy` entries: 52-byte body
program/readback followed by 4-byte commit and full record verify.

Three distinct physical mutations are supported internally:

- **Grant:** only into an empty/revoked slot, nonzero Gateway ID,
  no other active slot already holds that ID. Allocate a fresh
  **global** generation strictly greater than all four slot HWM
  values. No implicit replacement of an active Gateway.
- **Revoke:** require exact currently active Gateway ID and slot,
  store ID=0 but preserve an increased generation tombstone.
  Repeated or wrong-ID revocation is rejected.
- **Floor advance:** require `new_floor > current_floor` and
  retain all Gateway generations and IDs. No decreasing/resetting
  the global floor.

Every change increments global `policy_revision` exactly once.
Overflow/sentinel values and invalid slot or credential state
are refused BEFORE any flash mutation. Every attempted change is
rejected while another mutation/result is pending or the store
is not an active recovered v3 authority.

**Critical publication rule:** An accepted staging request is not
a successful grant. `v3_snapshot_` is updated only after a
physical committed record has been verified. A private
`takeV3PolicyResult(committed)` transfers the one-shot durable
result to a future trusted owner; it never accepts a remote ACK.

**Power-loss rules:** An ambiguous write, readback mismatch or
unknown SoftDevice completion locks SecurityStore into FAULT for
that boot. Boot journal recovery alone decides whether the policy
record was committed. No same-boot guessing/retry/authorization.
The existing SecurityStore A/B preflight rejects policy/tombstone
rollback across generations.

**Compaction:** If the v3 page lacks space for another policy entry,
the existing SF5K3H writer first copies the FULL previously
committed v3 snapshot to the opposite physical page unchanged;
only after the new page is verified/committed does the pending
policy record append. This prevents mutation from escaping into
the snapshot before it was durably recorded. Revocation
tombstones remain present through migration, compaction, reboot.

## Deliberately NOT implemented

There is **NO public grant/revoke API**, production caller, admin
key/signed command verifier, Gateway ACK admission/readout, relay
ACK proxy, custody release, BLE grant endpoint or RF control
surface in this PR. The private mutation method is exercised only
via `SecurityV3PolicyTestSeam`, defined in the host-only test
file; production source declares only the friend name.

Gateway policy storage is groundwork for later **authenticated
administrative admission** with credential lifetime, replay,
signature/MAC and authorization scope. Do not mistake durable
storage of a Gateway ID for proof that a live ACK frame is
authentic. Relay remains forward-only, not Gateway authority.

## New direct real-Store host cases

`firmware/tests/sf5/test_sf5k3j_real_security_v3_policy.cpp`
uses a physical 8192-byte NOR-backed fake, actual
`SecurityStore::poll`, and the v3 preflight reader. It tests:

- Real v2 provision→explicit v3 migration→grant/revoke/floor.
- Invalid slot, no implicit active replacement, duplicate IDs,
  wrong ID/slot revocation, floor and generation monotonicity.
- Staged but uncommitted mutation has NO in-memory/flash
  policy authority, one-shot durable-success result.
- Full record readback and normal reboot retaining exact
  Gateway tombstones, revision and global floor.
- Torn record body versus physically completed but
  backend-reported-failed commit; same-boot FAULT, reboot
  recovers precisely the persisted transaction.
- Filling the real 71-entry journal via TX counter reservations,
  then policy-driven A/B compaction, preserving tombstones,
  durable policy state and post-compaction reboot.

For short iteration (only the NEW test, not SF5K3A-I):
```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3j_policy_host.sh
```

The focused and full host suites also include this test,
but prior suites need not be rerun after every small change.
GitHub CI and RAK4630 target compilation remain distinct
release gates, as do future real hardware flash/power-cut
qualification and the authenticated-command owner review.

**Keep stacked draft PR, main unchanged and no field flashing.**
