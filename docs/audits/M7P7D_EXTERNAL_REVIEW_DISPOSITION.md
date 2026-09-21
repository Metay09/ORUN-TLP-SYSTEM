# M7P7D External Independent Review Disposition

Date: 2026-09-21
Baseline: `main@6774e7425a3776987ddaaff749c01d5cb20474c1`
Branch: `feat/m7p7d-app-request-seam`

Source: owner-provided independent Claude final audit. The reviewer worked from a
scratchpad copy, did not modify the repository, and independently ran the host
suite plus a pinned GNU++11 compile check.

Initial external result: **PASS WITH FIXES**.

This file records ORUN's disposition. It is not hardware evidence.

## Review environment note

The reviewer reported local/remote branch head `c2232d6` and absence of later
validation-document commits. Those later docs commits (`74c68ca`,
`a242b42`) were pushed after the owner's earlier fetch/validation window; the
GitHub branch subsequently contained them. The substantive firmware under review
through `c2232d6` was the same M7P7D implementation before the fixes below.

The owner validation that produced the original 22,116-byte RAM / 226,148-byte
Flash numbers was performed on the pre-fix implementation at `c2232d6`.
Post-review code fixes therefore require a fresh host/build pass before merge.

## F1 — config readiness vs durable provenance

External severity: MEDIUM.
Disposition: **ACCEPT AND FIX IN CODE + DOCS**.

The reviewer correctly observed that `ConfigStore::ready()` means the backend
and store initialized. It does **not** mean recovery found a committed config
record: blank or corrupt pages intentionally fall back to defaults while
`ready()==true`.

Fix:
- `ConfigStore::hasCommittedRecord()` exposes the already-owned
  `active_page_ >= 0` fact without creating a second state owner;
- the application response now separates:
  - `config_backend_ready`;
  - `config_has_committed_record`;
- USB output reports `source=stored|default`;
- focused tests include corrupt-page recovery.

No persistent bytes, recovery policy or flash write behavior changed.

## F2 — one response slot has no requester/transport identity

External severity: MEDIUM.
Disposition: **ACCEPT AS NEXT-ADAPTER GATE; NO CURRENT RUNTIME DEFECT**.

Today USB is the only application adapter, so one result slot cannot be consumed
by the wrong transport.

Before a second adapter (especially BLE GATT) is added, the request/result owner
must gain either a requester/adapter identity or adapter-specific bounded result
ownership. The exact shape is intentionally not frozen now.

This gate is recorded in M7P7D remaining work.

## F3 — validation evidence / SHA mismatch

External severity: MEDIUM.
Disposition: **ACCEPT PROCESS REQUIREMENT; TIMING DISCREPANCY EXPLAINED**.

Validation evidence must state the exact tested SHA. Earlier owner host/build
evidence applies to pre-fix `c2232d6`; the later docs commits did not change the
firmware, but the external fixes do.

Therefore the branch must be revalidated after these fixes. Final M7P7D closure
will record the exact post-fix head plus host/build numbers. No previous result is
silently promoted to the new code.

## F4 — startup test manually drained instead of proving loop composition

External severity: LOW.
Disposition: **ACCEPT AND FIX**.

Startup coverage now injects `APP CONFIG?` and calls the real production
`loop()`. It asserts the response slot is drained and the expected APP result is
emitted. Removing `drainApplicationResponse()` from production loop must now
break the test.

The BUSY scenario is also driven through the real loop.

## F5 — BUSY request id later reused

External severity: LOW.
Disposition: **ACCEPT AND FIX**.

Rejected BUSY work no longer prints a correlation ID. Only accepted requests own
an ID, eliminating ambiguous `BUSY id=N` followed later by
`RESULT id=N`.

## F6 — parser/id-wrap test gaps

External severity: LOW.
Disposition: **PARTIALLY ACCEPT AND FIX CHEAP GAPS**.

Startup coverage now checks:
- `APP CONFIG`;
- `APP CONFIG??`;
- lowercase `app config?`;
- `UINT32_MAX -> 1` local request-id wrap.

The internal unsupported-result print branch remains indirectly outside the USB
surface because USB has no command that can create an unknown typed request.
The focused service test already proves unknown request -> UNSUPPORTED with no
side effect. No test-only production command is added merely to cover printing.

## F7 — no code-level BLE-callback guard

External severity: LOW/INFO.
Disposition: **ACCEPT AS GATT IMPLEMENTATION GATE**.

Current production has no application GATT callback and the existing Bluefruit
callback only performs bounded event-counter handoff. Adding an artificial
runtime/thread guard now would be speculative.

The next BLE adapter milestone must prove callback -> bounded copy/enqueue ->
loop-owned dispatch and must not directly call application execution.

## F8 — no config generation / pending-save state

External severity: INFO.
Disposition: **DEFER**.

Current M7P7D is a synchronous read-only snapshot and production has no application
config-write caller. Future asynchronous mutation can extend result state without
rewriting today's owner boundary. No speculative state machine is added now.

## Additional independent evidence

The external reviewer reported:

- full host suite in a scratchpad copy: PASS;
- M7P7D unit test under `-std=gnu++11` + ASan/UBSan: PASS;
- pinned ARM GCC 7.2.1 compilation of `application_request.cpp` and
  `config_store.cpp`: PASS;
- no production PIO size rebuild, intentionally, to avoid repository writes;
- no TLP/storage/RF/BLE-admission/security/mixed-fleet regression found;
- no physical hardware test required for the read-only USB scope.

## Merge state

All code/document findings F1–F5 and the cheap F6 gaps are addressed on the same
branch. F2/F7 remain explicit next-adapter gates, not current defects.

**Post-fix host + RAK4630 build revalidation is still required before final PASS
and merge.**
