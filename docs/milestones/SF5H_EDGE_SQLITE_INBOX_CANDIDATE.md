# SF5H — Edge SQLite Exact Ciphertext Inbox (host-only candidate)

Date: 2026-10-14
Status: **CANDIDATE — exact-HEAD host tests pending; no production authority**.
Base: `main@56b00a2cc0e81bdccfc2d8c95eaa730a16aa206d` (PR #125).

## Purpose / consistency with SF4A and SF5

An Edge is the larger local durable queue behind a fixed or roaming
Gateway. It may be an Android phone or a connected Linux/OpenWrt host;
its connectivity, receipt handling and backend sync remain separate.
The Gateway flash queue protects tracker responsibility across
short disconnects; Edge manages longer connectivity loss.

The exact `EDGE_DURABLE_ACCEPT` authenticated wire/channel, issuer
credential, epoch/replay and timeout rules remain unapproved.
**No amount of SQLite success can be promoted to an authenticated
EDGE_DURABLE_ACCEPT, GATEWAY_CUSTODY_ACK, or permission to reclaim** until
these separate security and reset-safe boundaries are implemented.

This slice implements only a transport-neutral **host disk storage
candidate** at `edge/candidates/edge_durable_inbox.py` plus the
reproducible standard-library regression at
`firmware/tests/sf5/test_sf5h_edge_durable_inbox.py`.

## Current candidate behavior

The Python 3 built-in SQLite database uses `journal_mode=WAL`,
`synchronous=FULL`, a single `BEGIN IMMEDIATE` transaction per
object, an indexed SHA-256 digest plus packet length, and a retained
BLOB containing the entire **exact 84B EVENT / 100B PERIODIC sealed
PRODUCT_SECURE custody-requested object**. These packet structural
checks do **not** decrypt or authenticate origin, payload, or routing
information. A 56–96B delegated RESULT is NOT accepted yet.

On `put(object)`:

1. Reject invalid object shape/size, unbounded configuration or
   in-memory/URI database path; accept only an explicit disk path.
2. Lock transaction; check indexed candidates' digest, full exact
   bytes and length. Digest is an **index not unique evidence**:
   different objects with a simulated same digest are stored
   separately; inconsistency of indexed digest and current bytes
   causes a fail-closed rejection.
3. Return DUPLICATE for an existing exact object, even if capacity is
   exhausted. Otherwise refuse at configured `max_objects` (default
   4096; maximum accepted configuration 100,000). NEVER evict.
4. Insert exact bytes; commit; open a separate SQLite connection and
   read the object back to verify same length/digest/exact ciphertext.
   Only then return COMMITTED (host-local storage assertion).

The sequence is a local SQLite primary key for bookkeeping, NOT a
globally trusted object identity, protocol field or custody token.
Any error, busy/locked store, loss of readback certainty or injected
failure returns **UNCERTAIN**; a later retry can reconcile by exact
duplicate. The database has no delete, reorder, release, network
listener or receipt-generation API.

**COMMITTED is not physically proven durability**: it depends on
SQLite's SQLite/fsync/filesystem/kernel/hardware assumptions. Power
loss may behave differently; we test SQLite reopen and injected
transaction failures only. No storage or gateway authorization is
created by an untrusted local caller.

## Host regressions

* 84B EVENT and 100B PERIODIC exact bytes survive a fresh SQLite open.
* Same bytes idempotent; different bytes committed; max capacity
  never evicts; existing duplicate still recognized when full.
* Before-commit injected exception rolls back; after-commit unknown
  result survives restart and retry becomes duplicate.
* Simulated postcommit readback failure yields UNCERTAIN, not success.
* Forced SHA-256 collision still distinguishes full exact bytes.
* Deliberately corrupted indexed BLOB refuses instead of treating a
  digest hit as an exact duplicate.
* Invalid shape, best-effort flags, missing custody-request bit,
  forbidden sizes/families, invalid epoch/counter, bad paths and
  unbounded capacities are rejected.
* WAL mode and FULL synchronous setting are checked.

## Still missing — no privileged operation is approved

1. **Real Edge authentication and enrollment:** verify the receiving
   Gateway and exact object identity, mitigate malicious storage
   flooding, and bind acceptance to authorized devices.
2. **Real EDGE_DURABLE_ACCEPT:** freeze a reviewed cryptographic
   receipt/wire or authenticated local-channel application result,
   counters/nonce/replay, keys, rotation, issuer identity, and bounded
   pending-attempt policy. A DB row or HTTP 200 is not such a receipt.
3. **Phone/host implementation:** Android service and host transport
   are intentionally not included. Later adapters may reuse the
   schema *only after* reviewing lifecycle, permissions and OS
   storage APIs; SQLite is a candidate, not a mandate.
4. **Gateway trust and timing:** enforce verified exact receipt,
   persistent anti-replay and gateway flash fallback racing within
   tracker rendezvous. `kCandidateOnly` from host SF5G may not
   trigger tracker ACK.
5. **Edge -> Backend:** authenticated durable backend acceptance,
   idempotency/failure/retry and eventual secure Edge reclaim; a
   deletion API is deliberately absent here.
6. **Physical and security qualification:** database corruption,
   actual power-cut/fsync semantics, disk-full/storage removal,
   concurrent readers/writers, readonly filesystem, process crash,
   upgrade/backup/restore, quota/performance, and the agreed
   independent audit at the later storage-critical integration point.

Do not confuse host SQLite behavior with the RAK4630 product-region
NOR model (PRs #121–#125); no new RAK firmware, flash erase, Gateway
runtime, radio bytes or authenticated custody ACK are part of this PR.

Validation gate: run full `bash firmware/tests/run_host_tests.sh` on
the exact feature commit; look for SF5H SQLite inbox PASS in addition
to eight existing SF5G PASS lines.
