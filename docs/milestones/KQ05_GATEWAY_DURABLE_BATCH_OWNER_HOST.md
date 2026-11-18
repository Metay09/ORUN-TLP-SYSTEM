# KQ05 actual OCQ2 durable readback -> five-object Gateway batch HMAC

**Status: DRAFT / PC HOST TEST PENDING. NOT READY TO FLASH OR RELEASE.**

Parent PR #345 at `ed66c747dfa07ba7d0a554908d998c00ba9430fe`:
the user verified 1..5 exact object SHA256, sorted-prefix HMAC,
tamper and independent Python oracle **PASS (HOST CRYPTO ONLY)**.
Parent PR #343 OCQ2/v4 Gateway ID and segmented 39-page state likewise
passed its specific PC host test. Nothing in this slice claims a real
device image, LoRa airtime or accepted backend delivery.

## Why

A valid HMAC of five arbitrarily supplied RAM frames is **not** a Gateway
custody receipt: a Gateway may promise custody only after an exact
ProductCustodyStore record is committed to nonvolatile Flash, read back
and still held (or durably handed off). The existing single-object
GatewayLocalCustodyOwner already uses this principle but its ingress
is deliberately single-pending. The KQ05 group needs a separate,
bounded proof.

## Bounded implementation

`Kq05GatewayDurableBatchOwner` requires:
1. One 1..5 **explicit ProductCustodyStore::Handle** group, no duplicate
   handles, no range or high-water inference. It does not use the
   Tracker's **four exact-object cache** to collect Gateway records.
2. A currently ready, nonbusy, nonfaulted ProductCustodyStore, with no
   outstanding unconsumed custody/maintenance outcome.
3. One trusted `GatewayCustodyGrantProvider` whose local provisioned
   Tracker, physical Gateway ID, epoch, floor and grant generation
   match the group. The 32-byte scoped `K_custody_ack` is never
   accepted directly from LoRa and is zeroed after use.
4. `ProductCustodyStore::readConfirmedDurable()` for **EVERY** named
   handle (includes previously durably handed-off records solely so a
   lost ACK may be re-sent). If any lookup is missing, torn, foreign,
   still pending or corrupt, deny the whole group, not just that item.
5. The existing private 1..5 cryptographic signer generates a
   full-digest, distinct-domain HMAC. Before publishing bytes, this
   owner **rechecks** the local grant/epoch/floor/generation/key and
   **re-reads each exact OCQ2 object**, refusing any unexpected change.
6. No write or erase in the new owner. Output stays unchanged on
   failure. `kReady` means only an ACK frame constructed in memory;
   it is **NOT** transmission or delivery.

## Host-only tests

`bash tools/ci/run_kq05_gateway_durable_batch_owner_host.sh`

Uses the *actual* ProductCustodyStore production OCQ2 flash state
machine + real private batch SHA256/HMAC core, a fake NOR backend,
the existing OpenSSL host-only crypto provider, and a revocable
trusted-provider fixture.

Checks:
- Reject before record commitment, during pending Flash operation,
  and until the completed custody outcome is consumed.
- Commit 5 distinct periodic/event records, read back exact bytes and
  issue/verify 1..5 record selective batch HMACs without new writes.
- Reconstruct ProductCustodyStore after a fake power cycle and
  re-issue an idempotent five-record ACK from exact retained Flash.
- Reject unknown/torn/foreign Tracker record, duplicate handles,
  revoked grant, mismatched Gateway identity, changed epoch/floor/
  generation or key, and a corrupted OCQ2 CRC.
- No caller output or length changes on denial; no record erase,
  no physical device access and no RF.

## Still blocked — these are NOT solved by this PR

- Runtime receives and commits the **five** custody-requested objects
  one at a time, but no real Gateway loop/RF owner currently accumulates
  the five *handles* for this new batch owner and handles partial
  groups, bounded batching deadline, fairness with latest LIVE,
  backlog/replay or multiple Trackers.
- Target Tracker exposes a **four** cached exact-object snapshot
  through `ProductRecordStore`. The Tracker still needs a verified
  bounded five-object matcher (without arbitrary record release).
- Live Tracker `SecurityStore` grant derivation, signed group verifier,
  per-record durable flash release, multi-Gateway lost ACK/restart and
  LoRa TX/RX framing are not yet implemented for this new type.
- This owner takes a trusted Gateway grant provider and separately
  supplied physical Gateway ID as **composition preconditions**.
  It does not itself attest hardware identity or install durable
  credentials, and cannot replace the role/History region cutover.
- No field release; P0 remains **BLOCKED**. No automatic erase of
  legacy 28KiB and no firmware upload.

The next integration slice is bounded **Gateway loop handle collection
and Tracker 5-record authenticated release**, not reducing the number
of cached Tracker records by blindly deleting them.
