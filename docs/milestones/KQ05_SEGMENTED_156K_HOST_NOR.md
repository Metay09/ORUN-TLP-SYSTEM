# KQ05 noncontiguous 128 KiB + 28 KiB: 39-page virtual NOR (HOST ONLY)

**Status: draft standalone host experiment; no physical cutover, no ACK
minting, no device firmware upload and no live capacity increase.**

Follows approved direction in ADR #335, read-only census #336 (PC PASS)
and dual-region decision table #337 (PC PASS).

## Existing physical map (unchanged)

- Primary: `0xC5000..0xE5000`, 32 x 4096 = 128 KiB.
- **Other owners — not free**: `0xE5000..0xED000` contains
  Geofence, Security, Config and Bond/InternalFS.
- Old History: `0xED000..0xF4000`, 7 x 4096 = 28 KiB.
- Logical 39-page map is 32 Product pages followed by 7 *remapped*
  legacy pages, **NOT a single contiguous physical erase partition**.

## Code in this experiment

`firmware/include/kq05_segmented_flash_host.h` is explicitly behind
`ORUN_KQ05_SEGMENTED_HOST_TEST` and a production include triggers a
compile error. It takes two separate `FlashBackend` instances and
maps logical byte/page operations without exposing the intervening
physical Flash. It refuses cross-seam and cross-page program operations,
out-of-range reads/erases and any mutation while another backend has an
in-flight async mutation. A failed/uncertain operation quarantines the
adapter. **It is not the production FlashMutationGate**, and the input
flags for role/exclusive ownership are simulated only.

The existing store limits (without changing runtime page counts) now
permit testing:
- `ObservationStore::kMaxPages = 39` (previously 32);
- `ProductCustodyStore::begin()` upper bound = 39 pages (previously 32).

Both product stores are still physically opened with **32 pages** by
current device composition. The product owner and no-legacy-writer
runtime transition have not changed.

### Integrated test: existing Gateway custody store

A fake NOR fills the 32 primary OCQ2 pages with exactly 33 valid held
objects each. The *real portable* `ProductCustodyStore` is then mounted
through the 39-page logical mapping. A new exact protected object is
submitted, committed/read-back through the existing OCQ2 state machine
and recovered after simulated reboot **in logical page 32, physical
supplemental page 0**.

This tests no invented OCQ2 queue. The actual product-custody format
and state machine perform the write and recovery in a synthetic Flash
backing, while no real hardware is touched.

Additional host checks: boundary refusal, cross-seam refusal, pending
mutation ownership, write failure quarantine and rejection of unknown
legacy History bytes. The test has not yet been run on user's PC.

`bash tools/ci/run_kq05_segmented_156k_host.sh`

The new script is also included in KQ05 LAB preflight.

## Outstanding engineering gates

1. This host adapter **cannot** and must not be used as a runtime Flash
   port. Provide a physically bounded role-aware shared
   `FlashMutationGate` with correct SoftDevice async owner
   reconciliation and cutover from `HistoryStore`.
2. The current Gateway OCQ2 page header has no Gateway-device ID.
   A durable role/device binding is mandatory, especially before
   reusing old History pages. A caller-supplied `true` is never
   sufficient production evidence.
3. Tracker needs an **integrated** 39-page ObservationStore
   append/sequence/release/reclaim host test, not merely the increased
   max-pages compile bound. Fully qualify cross-page crash recovery.
4. Gateway's full/handed-off page reclamation needs physical power-cut
   tests and proof no unresolved custody is destroyed. The test's fake
   primary pages are prefilled intentionally rather than taking hours
   to build real field throughput.
5. Implement a **new authenticated Gateway selective batch custody ACK**
   (not an unauthenticated or re-labeled KQ05 Backend receipt), including
   multi-Gateway/offline/idempotent release and traffic budget.
6. Prove operator-visible legacy migration with no custody liabilities,
   full PC regression, actual radio + power cuts, current/live first and
   unchanged other Flash partitions. Issue #221 remains BLOCKED.

These 39 pages provide **156 KiB nominal physical room per device**,
not 156 KiB confirmed usable observation/custody payload. Page headers,
control pages, incomplete slots and wear/reclamation reduce the net
capacity.
