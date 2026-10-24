# SF5L5 — bench Edge hand-off (confirmed delivery, Gateway reclaim)

**Status:** code + host tests. **Physical bench check: pending (owner).**
Bench only for authentication (public test key). The store mechanism is
production code.
**Base:** `main@c6bd9b4` (SF5L4 merged).

## Why

The Gateway kept every record until an operator ran `BENCH GW ERASE HELD`.
On 2026-10-24 that cost 6 records: the erase ran before the PC had them. The
Gateway also filled up at 1056 records (~11 h at the bench rate) and stopped
ACKing.

Architecture rule (`ORUN_GATEWAY_DURABLE_CUSTODY.md` §4.2): the Gateway may
reclaim only after an authenticated Edge durable accept bound to the exact
object. This milestone implements that loop for the bench.

## Mechanism

1. **PC tool:** after the ledger has committed a held record (SQLite
   `synchronous=FULL`; ledger result STORED or DUPLICATE), the tool sends
   `EDGE ACCEPT v1 page=<P> slot=<S> gen=<16 hex> mac=<64 hex>`.
   - One accept is in flight at a time.
   - BUSY or silence (3 s) → resend; UNKNOWN → done; REJECTED → reported and
     kept on the Gateway.
   - `--no-accept` disables this.
2. **Gateway** (`Sf5l0EdgeHandoff`):
   - Rereads the exact durable bytes at (page, slot, generation) and checks
     the MAC over SHA-256 of those bytes.
   - `ProductCustodyStore::requestHandoff` checks the bytes and the
     generation again, then programs ONE hand-off marker word. This is the
     existing OCQ2 format field: two marker slots, so a torn write is
     retried in the other slot. No rewrite, no erase.
   - Read back as `kHandedOff`. A repeat accept returns `ALREADY`.
3. **Reclaim:** `ProductCustodyStore::reclaimablePage()` /
   `requestReclaim()`.
   - A page is erased only when **every** record on it is handed off, and
     never while it is the page still accepting records.
   - The erase is verified. If it was the newest page, the next admission
     opens a fresh page with a higher generation.
   - Lines: `EDGE RECLAIM page=P started|OK|FAIL`.
4. **Lost ACK:** a Tracker resend of a handed-off object that has not yet
   been reclaimed is re-ACKed from the same durable bytes
   (`readConfirmedDurable`).
   - This supersedes SF5K3P's "no ACK for handed-off". Without it, a lost ACK
     pins the object in the Tracker's 4-slot custody cache until ring
     retention erases it.
   - After reclaim, a resend is admitted again as new custody, and the
     Edge's dedupe converges it.

MAC: `HMAC-SHA256(K_edge, M)`.
- M is 102 bytes: `"ORUN-EDGE-ACCEPT-v1"` ‖ gateway id ‖ page ‖ slot ‖
  generation ‖ SHA-256(object) ‖ zero padding.
- K_edge = HKDF(salt = test credential id, ikm = test root, info = 57 bytes:
  `"ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1"` ‖ gateway id ‖ zero padding).
- The fixed sizes reuse the size-locked CC310 primitives unchanged.
- **Bench only:** the key derives from the public test credential, so this
  proves exact-object binding, idempotence and reclaim, not the Edge's
  identity. Production needs an enrolled Edge key (step 3, real security).

The bench Gateway's USB command buffer is 160 bytes (48 elsewhere).

## Evidence

- `test_sf5l5_edge_handoff.cpp` (ASan/UBSan, real store + owner + ingress,
  fake NOR with erase):
  - malformed, wrong MAC, wrong generation and MAC-for-other-bytes never mark
    anything;
  - accepts are idempotent; a second accept in flight is BUSY;
  - the page still accepting records is not reclaimed even when all its
    records are handed off;
  - a full page is erased right after its last accept, and an accept for a
    reclaimed record is UNKNOWN;
  - a handed-off duplicate is re-ACKed;
  - after reboot, mount works and the reclaimed page is reused.
- `test_sf5l5_edge_handoff.py`:
  - the same MAC vector, computed independently in Python
    (`cryptography`);
  - queue only for verified records; BUSY/timeout retry; REJECTED not
    retried; `--no-accept`.
- Full host suite PASS. `rak4630` unchanged (362 912 B). Gateway bench
  380 496 B.

## Known limits

- A power cut in the middle of a page erase leaves a page that refuses to
  mount (fail closed; recover with `BENCH GW ERASE PRODUCT CONFIRM` after the
  PC has the records). Nothing is lost silently.
- After every page has been reclaimed and the Gateway reboots, generations
  can repeat. Accepts also bind the exact bytes, so a stale accept cannot
  mark a different object.
