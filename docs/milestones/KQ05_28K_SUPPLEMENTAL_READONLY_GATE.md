# KQ05 28 KiB supplemental area: first read-only guard (PC only)

Status: **DRAFT / NON-MUTATING / NOT A DEVICE MIGRATION**.  
Architecture: `docs/architecture/ADR_KQ05_OFFLINE_SUPPLEMENTAL_28K_GATEWAY_BATCH_CUSTODY.md`.

The full legacy History region on **each device** is
`[0xED000, 0xF4000)`: 7 independent 4 KiB pages, 28 KiB total.
The existing 128 KiB Product region remains physically separate and
untouched. There is no 8 KiB receipt-only partition in this direction.

This slice adds `kq05_supplemental_28k_census`:
- Scans every byte in every legacy page with **read-only** 64-byte chunks.
  A blank page header plus nonblank payload is not a blank page.
- Refuses `Unassigned`/`Conflict` device roles and zero/invalid identity.
- Reads the **existing** OBS1 Tracker and OCQ2 Gateway page headers using
  their real validators, without writing, initialising or erasing anything.
- Refuses mixed/wrong-role, unknown old-History, foreign Tracker identity,
  torn/incomplete header and storage-read failures.
- Gateway OCQ2 headers **do not include Gateway ID**; a structurally
  compatible page is reported as **NEEDS BINDING** and is never treated as
  permission to mount it or issue ACK.
- A Tracker OBS1 header passing basic identity checks is only
  **POTENTIAL** data. Full record/control recovery, same-incarnation
  sequencing, release authority and duplicate reconciliation across the
  128/28 KiB stores remain unimplemented.
- The portable helper invokes no `FlashBackend::begin()`, `program()`,
  `erasePage()`, `pollPending()`, USB, BLE, LoRa or radio function.

**Nominal ceilings only, not proven effective capacity:**
- Tracker: 7 pages total, but ObservationStore needs 2 control pages;
  5 data pages x 42 = **210 data slots** before reserve/reclaim effects.
- Gateway: 7 pages x 33 = **231 OCQ2 custody slots** before reserve/reclaim
  effects. This format currently lacks per-Gateway page identity binding.
- These numbers are not automatically reachable by the current firmware.
  The new code does not enlarge the live queues.

## PC-only gate

`bash tools/ci/run_kq05_supplemental_28k_census_host.sh`

Uses the actual portable OBS1/OCQ2 header encoders and decoders, fake 28 KiB
readable NOR, read failures and mixed-role/foreign/torn/old-page cases.
Also wired into `run_kq05_lab_preflight.sh`. Not yet executed by the
user's PC as of this draft.

## Next gates before any physical write

1. Develop a **separate role-aware 28 KiB FlashMutationGate port**, sharing
   the existing SoftDevice physical-flash arbiter; ensure exclusive ownership
   and no legacy HistoryStore mount/writes on any KQ05 role. Never simply
   hand the old History port to two active stores.
2. Decide and test **safe 28 KiB role+device binding** (mandatory for
   Gateway, as OCQ2 itself does not bind Gateway identity).
3. Implement the two-store coordination: Tracker must use one
   incarnation/sequence authority across both stores and apply custody
   release to the exact record's store. Gateway must check both custody
   stores, issue only authentic exact-object batch custody ACK, and
   wait for verified Edge/Backend durable handoff before retirement.
4. Crash/reboot, full/backpressure, page reclamation, simultaneous
   multi-Gateway acknowledgements, offline/disconnected PC integration
   and full existing host/build regression.
5. **Separate operator-approved one-time cleanout** of obsolete History
   after proving no live custody or foreign device data. No erase at boot.
6. Physical two-Gateway/one-Tracker testing, radio budget and real power
   measurements before any field-release claim.

This PR deliberately **does not activate** legacy-region writes, mint ACK,
change active firmware partitions, modify production DB/services or update
any connected physical RAK device.
