# SF5H — Read-only Edge FIFO and Whole-Queue Integrity (host-only)

Date: 2026-10-14
Status: **CANDIDATE / EXACT-HEAD HOST TESTS NOT YET RUN**.
Base: `main@34fe734a28cd0e549399ae30276c0e3e711dae5f` (merged PR #126).

## Purpose

The previously merged host-only Edge SQLite inbox can write retained exact
84/100-byte encrypted PRODUCT_SECURE frames and recognize exact duplicates,
but did not have a safe chronological read API for eventual backend drain.
`oldest_batch(limit)` is a **read-only candidate** for this next boundary.

It returns `ScanResult(READY, QueuedFrame[])` with at most 64 locally
sequenced full ciphertext objects, or one explicit fail-closed status:
`EMPTY`, `REFUSED`, `UNAVAILABLE`, or `INVALID`.

### Invariants

* Begin one SQLite read transaction/snapshot; scan the **entire**
  retained table in strictly increasing SQLite sequence order.
  Require positive strictly ascending sequence, valid 84/100-byte shape,
  length, indexed SHA-256 digest and exact byte content. A CRC or hash
  does not authenticate identity; checking it merely detects storage
  inconsistency under assumed honest local storage.
* Return at most the requested 1..64 oldest objects **only after** the
  entire table has been checked. A corrupted entry at the tail causes
  `REFUSED` and returns **no earlier apparently valid prefix**.
* Do not silently skip holes in physical sequence: gaps are legal
  SQLite auto-increment artifacts; descending/duplicate/nonpositive
  sequence is not. This does not invent a backend watermark.
* No data is modified by `oldest_batch`; results are snapshots and
  cannot prove that the subsequent network send succeeded. It uses
  the configured bounded `max_objects` as a guard against an
  unexpectedly oversized/foreign database.
* SQLite I/O and busy exceptions become `UNAVAILABLE`, never a
  fake empty list or an accepted receipt. Invalid batch sizes,
  including boolean/floating types, are rejected without reading.

The operation is intentionally O(number of retained objects), not a
production worst-case performance claim. Later optimize a reviewed
durable index without weakening whole-queue integrity guarantees.

## Tests

`firmware/tests/sf5/test_sf5h_edge_fifo_readonly.py` runs in the full
host suite and covers:

* Exact FIFO contents and ordering across DB reopen, full capacity
  and idempotent duplicate admission.
* Invalid object at the LAST row blocks a requested one-item prefix.
* Modified stored ciphertext with stale digest blocks output.
* Invalid PRODUCT_SECURE shape **even if its digest is re-computed**.
* Nonpositive local sequence fails closed.
* Invalid limits, locked/unavailable DB and removed table fail closed.

### Non-goals and production gates

* No authenticated Edge receipt, backend transport, authenticated backend
  durable acceptance, replay/nonce authority or Gateway custody ACK.
* No delete, handoff, retention checkpoint, concurrency/reservation or
  reclaim API. This is **not yet** an Edge -> Backend worker.
* No real SQLite corruption adversary defense, filesystem proof,
  physical power-cut proof, encryption key policy, recovery migration,
  mobile Android lifecycle or storage wear/load measurement.
* Source files are confined to `edge/candidates/` and
  `firmware/tests/`; no RAK4630 firmware or hardware flash changes.
* Future backend sync must persist authenticated Backend durable
  acceptance before advancing any Edge release, and ensure the
  selected stored object is still the one accepted; a simple HTTP 200
  or socket send is not acceptance.
* Do not authorize Gateway reclaim or tracker release from SQLite rows.
  The separately reviewed `EDGE_DURABLE_ACCEPT` security contract is
  still mandatory.

## Merge gate

Run the full `bash firmware/tests/run_host_tests.sh` on the exact head.
Require `HOST_EXIT=0`, eight SF5G PASS, **two SF5H PASS** (SQLite inbox
and FIFO reader), and no traceback/assertion or sanitizer errors.
