# M7P7E — Application requester / response ownership

Status: **MERGED — PR #34 merged to main at `cb1e181f88ed8d8362f6d4d2f97b96734474c954`; external review PASS WITH FIXES, accepted fixes applied, post-fix host/build revalidation PASS. NO BLE APPLICATION GATT, PROVISIONING OR PROTECTED WRITE PATH.**

Baseline: `main@2cfb68b29458d3815f55f8df39d45faac64b2de6` (PR #32 / M7P7D merged).
Branch: `feat/m7p7e-requester-ownership`.
Merge: PR #34 -> `main@cb1e181f88ed8d8362f6d4d2f97b96734474c954`.

## 1. Why this slice exists

M7P7D deliberately used one bounded application response slot and only one
adapter (USB). Its own remaining-gates section requires explicit requester /
adapter response ownership before a second application transport exists;
otherwise USB could consume a BLE result, BLE could consume a USB result, or a
disconnected adapter could leave the global slot permanently busy.

M7P7E closes only that ownership seam. It does **not** add the second adapter.

This slice is intentionally independent of the currently open M7P6E corrected
fresh-pairing physical rerun. No BLE security event, CryptoCell operation,
pairing, bond mutation or hardware upload is required to validate this code.

## 2. Scope

M7P7E adds:

- explicit typed `ApplicationRequester` identities for the existing USB adapter
  and the future BLE adapter;
- requester identity carried from accepted request to response;
- requester-qualified response consumption;
- fail-closed cross-requester behavior: a requester cannot consume or clear
  another requester's pending result;
- requester-owned response discard so a later disconnected transport can abandon
  its own result without wedging the global bounded slot;
- host coverage for same numeric request IDs in different requester namespaces,
  cross-requester take rejection, global BUSY backpressure and owner-only discard.

The existing USB `APP CONFIG?` behavior and log shape remain unchanged.

## 3. Ownership semantics

The service still owns exactly one result slot.

```text
USB adapter --------------------+
                                |
future BLE adapter -------------+--> ApplicationRequestService
                                      |
                                      +--> existing ConfigStore
```

A request is identified by the pair:

```text
(requester, request_id)
```

`request_id` is therefore local to an adapter/requester namespace. Numeric IDs
may repeat between USB and BLE without becoming the same logical request.

`ApplicationRequester` is **local adapter provenance**, not a user/account identity,
authentication principal, BLE connection identity or wire field. A transport adapter
must assign its own constant requester value locally. Future peer-controlled BLE/USB/
LoRa bytes must never be allowed to select `kUsb`/`kBle`; doing so would defeat the
ownership boundary.

The single global slot remains intentional bounded backpressure. If any accepted
response is unread, every subsequent submit returns `BUSY`, regardless of
requester. No heap, generic event bus or unbounded per-transport queue is added.

Only the requester that owns the pending response may:

- take it; or
- discard it.

A mismatched take/discard leaves the pending response untouched.

The discard API is a lifecycle/recovery primitive, not an authorization bypass.
A later BLE adapter is expected to invoke it from loop-owned disconnect cleanup,
not directly from a Bluefruit callback.

## 4. Security boundary

M7P7E does not change the M7P7C/M7P7D security boundary:

- BLE connection != BLE bond != ORUN application authorization;
- no credential provisioning path is added;
- no `K_root` readback exists;
- no protected configuration write is added;
- no secure RF envelope or command path is added.

The future BLE GATT/commissioning slice must still define exact framing/UUIDs,
bounded MTU behavior, callback handoff, rate limiting, commissioning/authentication
ceremony, authority-key custody/recovery and Just Works/bond-store denial-of-service
behavior before protected writes are exposed.

## 5. Compatibility / system impact

```text
TLP v1 bytes/sizes:             unchanged
RF PHY/airtime/forwarding:     unchanged
HistoryStore bytes:            unchanged
ConfigStore bytes:             unchanged
SecurityStore bytes:           unchanged
BLE runtime/admission:         unchanged
BLE application GATT:          not implemented
provisioning/authorization:    not implemented
durable config write over APP: not implemented
USB APP CONFIG? text shape:    unchanged
MESSAGE/commands:              not implemented
Android/backend:               not implemented
```

The change is an internal C++ ownership contract only. It creates no new durable
state and performs no flash, radio, BLE or CryptoCell operation.

## 6. Validation evidence

Owner-run validation on exact branch head
`b4b13759942a890526b77167ed1b1c30f94d802a`:

- full `firmware/tests/run_host_tests.sh`: **PASS**;
- the runner reached its final R4 checks, so the silent M7P7D/M7P7E application
  request test completed successfully under the runner's existing
  warnings-as-errors + ASan/UBSan configuration;
- all production startup scenarios reported PASS;
- no hardware test or upload was performed for this slice.

Owner-run production build after the docs-only host-evidence commit
`5a4b4146b15944a7846b54f1de2665affc7f4717` (firmware code unchanged from
the tested code-bearing head):

- `pio run -d firmware -e rak4630`: **PASS**;
- RAM: **22,124 / 248,832 bytes = 8.9%**;
- Flash: **226,244 / 815,104 bytes = 27.8%**;
- delta versus merged M7P7D production baseline
  (22,116 RAM / 226,212 Flash): **+8 B RAM / +32 B Flash**;
- `check_exclusive_owner` completed without aborting the build;
- `check_application_ceiling` completed without aborting the build;
- no physical upload/test was performed.

The +8 B RAM / +32 B Flash delta is consistent with the requester tag and
owner-qualified response/discard seam; there is no new queue, heap allocation,
persistent record or transport runtime.

## 7. Remaining merge gates

Completed:

- full host/sanitizer/warnings-as-errors regression;
- production startup scenarios;
- production RAK4630 build;
- RAM/flash comparison;
- application-ceiling and exclusive-owner build guards;
- bounded diff review for protocol/RF/storage/BLE-runtime side effects.

Independent review result:

- verdict: **PASS WITH FIXES**;
- no BLOCKER/HIGH/MEDIUM findings;
- LOW invalid-requester slot orphaning: fixed fail-closed;
- LOW future BLE reconnect/stale-response lifecycle: accepted as a mandatory
  adapter-slice gate, not solved by speculative service-level session machinery;
- LOW edge-case test gaps: strengthened in unit and production-loop startup tests;
- disposition: `docs/audits/M7P7E_EXTERNAL_REVIEW_DISPOSITION.md`.

Post-review owner revalidation on exact branch head
`35f6cefe3d293d1309e3f9d203303a418c3da120`:

- full `firmware/tests/run_host_tests.sh`: **PASS**;
- all production startup scenarios: **PASS**;
- warnings-as-errors + ASan/UBSan host runner completed through the final R4 checks;
- the strengthened M7P7E requester, invalid-requester and startup-composition
  coverage therefore passed on the corrected code.

Post-review production build revalidation after docs-only head
`d3cdd7321aba52d18cf361902c9603d0a2707e94` (firmware code unchanged from
the corrected code head):

- `pio run -d firmware -e rak4630`: **PASS**;
- RAM: **22,124 / 248,832 bytes = 8.9%**;
- Flash: **226,292 / 815,104 bytes = 27.8%**;
- delta versus merged M7P7D production baseline
  (22,116 RAM / 226,212 Flash): **+8 B RAM / +80 B Flash**;
- delta versus the pre-review M7P7E build
  (22,124 RAM / 226,244 Flash): **0 B RAM / +48 B Flash**;
- `check_exclusive_owner` completed without aborting the build;
- `check_application_ceiling` completed without aborting the build;
- final production build result: **SUCCESS**.

All M7P7E merge gates are now closed.

No physical hardware test is required for M7P7E itself because it adds no BLE
application runtime, driver behavior, persistence mutation or RF behavior.

The separate PR #33 M7P6E corrected fresh-pairing hardware rerun remains open
and must not be reclassified as closed by this milestone.

## 8. Next gate

After M7P7E post-fix revalidation/merge, the next BLE application slice may
define the exact bounded GATT transport contract and commissioning/authentication
design. It must preserve callback -> bounded handoff -> loop-owned application
dispatch, and it must not expose protected writes merely because a client is
connected or bonded.

Because `ApplicationRequester::kBle` names the adapter rather than one BLE
connection, that slice must also close transport-session hygiene without turning
connection state into identity/authorization:

- disconnect cleanup for the old connection must complete before a replacement
  connection can submit application work;
- stale cross-connection delivery must be prevented with bounded adapter-local
  correlation state (for example non-resetting request ids and/or a local
  session discriminator);
- the adapter must take a completed application response promptly into its own
  bounded output buffer rather than hold the global application slot until the
  remote client chooses to read;
- disconnect/reconnect, delayed cleanup, stale response and non-reading/flood
  cases require host coverage.

No service-level multi-session framework is added in M7P7E because no BLE
application adapter exists yet.
