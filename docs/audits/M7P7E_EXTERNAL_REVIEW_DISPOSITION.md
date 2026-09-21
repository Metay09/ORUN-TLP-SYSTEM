# M7P7E External Review Disposition

External review target: `dd1e40b97002000d72b9ad9406e56a0334a57940`
Baseline: `main@2cfb68b29458d3815f55f8df39d45faac64b2de6`
Reviewer verdict: **PASS WITH FIXES**
Review date: 2026-09-21

The independent reviewer inspected the requested architecture/security records,
the complete baseline-to-head diff, production/request-service code and tests.
It also independently ran the host suite and production RAK4630 build, reproduced
the owner resource numbers, compiled the seam under pinned ARM GCC 7.2.1 GNU++11
with warnings-as-errors, and mutation-tested the requester ownership checks.

No BLOCKER, HIGH or MEDIUM finding was reported.

## 1. LOW-1 — invalid requester could orphan the response slot

**Accepted and fixed.**

The reviewer demonstrated that an out-of-domain
`ApplicationRequester` value such as 0 or 0xFF was accepted, copied into the
pending response and then could not be consumed/discarded by either supported
adapter. That made the global one-slot backpressure unrecoverable through the
supported requester API until reboot.

Fix:

- `ApplicationSubmitResult::kRejected` was added;
- `ApplicationRequestService::submit()` now rejects any requester other than
  `kUsb` or `kBle` before creating a pending response;
- the production USB adapter handles the impossible/internal `kRejected` result
  explicitly without consuming a request id;
- host tests cover 0 and 0xFF requester rejection, an unchanged empty slot and a
  subsequent valid request.

Code-fix commits:

- `c54d76bf18904bb7a73047e26082c5228546d0d5`
- `21043860ce49ad18cf3fcc604fd792b585db6ae1`
- `8513bcc6ec561f4f173ebf087eb5077735b35dba`
- `a5514d1797297a96b1adbd3d4fa690e333fb53d0`

## 2. LOW-2 — future BLE reconnect/stale-response lifecycle was under-specified

**Accepted as a future BLE-adapter gate; no speculative session framework added
to M7P7E.**

`ApplicationRequester::kBle` identifies the local adapter, not an individual
BLE connection. Therefore a future adapter must not let an old connection's
pending result be delivered to a later connection or let delayed disconnect
cleanup erase a newer result.

M7P7E does not add a BLE adapter, so the correction is architectural/documentary:
the first BLE application-adapter slice must prove bounded connection-lifecycle
hygiene before exposing the service. It must:

- process disconnect cleanup before admitting work from a replacement
  connection;
- prevent stale cross-connection delivery using adapter-local correlation
  state (for example a non-resetting request-id namespace and/or an explicit
  local session discriminator), without treating that state as identity or
  authorization;
- drain an application response promptly into a bounded adapter-owned output
  buffer rather than holding the global application slot until a remote client
  chooses to read it;
- host-test disconnect/reconnect, stale response, delayed cleanup and
  non-reading/flood cases.

This preserves the current single bounded application slot and avoids adding a
multi-queue/session framework before a real BLE adapter exists.

## 3. LOW-3 — requester ownership edge-case coverage

**Accepted and strengthened.**

Added direct coverage for:

- the symmetric direction (BLE cannot take/discard a USB-owned response);
- empty-slot take/discard returning false;
- wrong discard leaving the original response intact for the rightful owner;
- owner discard reopening the slot;
- production-loop composition where a BLE-owned pending response makes USB
  `APP CONFIG?` BUSY and the USB drain cannot steal the BLE result.

Startup-composition commit:

- `8329b9e21135229ab47102fe62288120ed72377a`

## 4. Notes disposition

The reviewer noted that the test helper performs `takeResponse()` inside an
`assert`; the current runner does not define `NDEBUG`, so this is not a current
correctness issue. No change was required.

The default `ApplicationResponse` requester remains `kUsb`; ownership is
guarded by `response_ready_`, so the reviewer identified no current bug. No
change was required.

## 5. Scope confirmation from independent review

The reviewer independently found no change to:

- TLP v1 bytes or compatibility fixtures;
- RF airtime, RX or forwarding;
- ConfigStore/SecurityStore/HistoryStore formats;
- BLE runtime/admission;
- security credentials, authorization or protected writes;
- flash ownership;
- power/sleep policy;
- valid USB `APP CONFIG?` visible behavior.

Pre-fix independent production build matched owner evidence exactly:

- RAM: 22,124 bytes;
- Flash: 226,244 bytes.

## 6. Post-fix gate

Because LOW-1 changed production code and LOW-3 changed host/startup tests, the
pre-fix validation cannot be promoted to the final code head.

Before merge, rerun:

1. full `firmware/tests/run_host_tests.sh`;
2. production `pio run -d firmware -e rak4630`;
3. record final RAM/Flash and confirm application-ceiling/exclusive-owner guards.

No physical hardware test is required for M7P7E itself. The separate PR #33 /
M7P6E corrected fresh-pairing physical rerun remains open and is not closed or
bypassed by this review.
