# KQ05 next verification: physical 128 + 28 KiB on existing FlashMutationGate

Status: **PC HOST-ONLY, pending user's execution**.
Parent: PR #339 (Tracker OBS1 + Gateway OCQ2 39-page fake NOR **PC PASS**
at `1918a1d4`).

## Why this step exists

The 39-page logical store tests proved data append/reboot and identity
continuity with two independent synthetic NOR backends. The real nRF52
uses a **single Nordic Flash/SoftDevice arbitration owner**,
`FlashMutationGate`. The new host gate links the actual existing:
- `FlashMutationGate::productPort()` for 128 KiB `0xC5000..0xE5000`;
- `FlashMutationGate`'s existing History `FlashBackend` interface for
  28 KiB `0xED000..0xF4000`;
- original `NrfProductFlash` and `NrfHistoryFlash` bounds, begin,
  synchronous program and async SoftDevice submission/completion logic.

The intervening `0xE5000..0xED000` remains untouched:
Geofence, Security, Config, relocated Bond/InternalFS. No 39-page
continuous *physical* NOR partition exists.

## New host gate

`bash tools/ci/run_kq05_physical_gate_dual_region_host.sh`

A native test maps fake NOR arrays at the real absolute nRF52 physical
addresses and uses Nordic SVC mocks with actual FlashMutationGate code:
1. Refuse begin without explicit simulated exclusive History shutdown.
2. Initialize Product + History ports before simulated SoftDevice on.
3. Program one word in each physical partition through the test-only
   segmented adapter. Refuse requests spanning the virtual seam,
   out-of-range erases and check all four protected partitions unchanged.
4. With SoftDevice enabled, submit History program, then independently
   stage Product program. Assert **one physical flash SVC call only**
   before History completion and that the second SVC begins only after
   its predecessor's completion event.
5. Refuse `HIST` legacy content at any History page before allowing a
   39-page mount. No physical wipe or rewriting.

This uses `mmap`, fake NRF_FICR/UICR, `sd_flash_*` and `sd_evt_get`
stubs **on the PC only**. The compile-time
`ORUN_KQ05_SEGMENTED_HOST_TEST` flag is still mandatory.
The existing production firmware composition is **unchanged at 32 pages**.

## What remains blocked

- No actual operator-approved legacy History cleanout or persistent
  one-owner migration was implemented. The host flags
  `exclusive_history_owner_stopped` and
  `device_role_identity_bound` are NOT actual runtime proof.
- The History-facing `FlashMutationGate` lacks the same explicit
  `hasUnreconciledMutation()` virtual override already exposed by
  Product/Config/Security ports; a physical segmented owner must add
  and qualify equivalent history quarantine visibility before runtime.
- Gateway OCQ2 page header does not bind Gateway device identity.
  A real durable local role/device proof must precede mount/ACK.
- Actual authenticated **Gateway batch custody ACK** still needs its
  own wire format, per-Tracker scope, exact hashes, selective release,
  multi-Gateway replay tests and RF budget. Do not equate a Gateway
  custody ACK with a Backend durable receipt.
- Real hardware 2+ Gateway / Tracker power cut, BLE/LoRa concurrency,
  storage pressure and functional/field gates are NOT passed.

No physical Flash write, device firmware upload, CI field approval,
or production service change in this PR. Do not merge it as a product
activation patch.
