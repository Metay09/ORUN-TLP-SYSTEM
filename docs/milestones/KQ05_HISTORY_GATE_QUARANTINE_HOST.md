# KQ05: History FlashMutationGate uncertainty and segmented 39-page ownership

**Date:** 2026-11-18  
**Status:** Proposed host-regression fix; requires execution on user's PC.  
**Parent:** PR #340 (`fe511779`), whose physical 128 KiB + 28 KiB address and arbiter host test passed on PC.  
**Scope:** No firmware role cutover, device upload, History wipe or batch custody ACK.

## Finding

Existing product/config/security/geofence `FlashBackend` ports
override `hasUnreconciledMutation()` and expose their respective
`FlashMutationGate` quarantine flags. The legacy `History` view is
the gate itself, but did not override the base class's always-false
virtual method.

Therefore a new 39-page segmented owner using the History view could
not see the precise case where the Nordic SoftDevice had **accepted** a
History write, the application had timed out awaiting the matching
completion event, and the gate had quarantined the history owner.
The physical Flash token was still retained by the gate until the
late event, which is correct, but the virtual storage-level diagnostic
was falsely clear. This is a fail-closed integration requirement.

## Patch

The History-facing gate now overrides
`FlashBackend::hasUnreconciledMutation()` to return
`history_slot_.quarantined`. This does not weaken its existing single
physical writer, Flash priority, Nordic event owner or timeout logic.
No writes, erases or on-device state transitions have been added.

New test in the existing actual-FlashMutationGate PC runner:

1. Boot the simulated Product+History physical Flash ports.
2. SoftDevice-accept a History program without supplying the
   completion event; reach the >4s application operation deadline.
3. Assert the timed-out segmented owner becomes faulted, and its
   FlashBackend view reports `hasUnreconciledMutation() == true`.
4. Stage a separate Product write. Prove no second Nordic SVC is
   submitted before the History completion event.
5. Deliver History's **late** success event. Verify it reconciles the
   Flash token and records a `late_completions` diagnostic, but does
   not turn the already failed History operation into an application
   success or revive the segmented store.
6. Verify the queued Product owner can now run and protected physical
   partitions remain unchanged.

Run on PC only:
`bash tools/ci/run_kq05_physical_gate_dual_region_host.sh`

## Still NOT established

- This test deliberately uses an **explicitly host-only** segmented
  adapter and simulated booleans for HistoryStore shutdown and local
  role/device binding. Runtime shutdown and durable source-of-truth
  for binding are not implemented.
- The `HistoryStore` currently exists in the firmware composition and
  must not mount alongside any KQ05 28 KiB supplemental owner.
- No automatic old-History data cleanup or Flash migration is allowed.
- No authenticated/selective Gateway batch custody ACK currently
  exists. Backend-durable receipt and Gateway custody remain distinct.
- Power cut, true BLE/LoRa activity and radio timing need physical
  verification before release.

**Field status remains BLOCKED.**
