# KQ05 Tracker: integrated 39-page ObservationStore host gate

**Status: host-only proposed regression. Not tested on user's PC yet.
No physical 28 KiB History cutover, Flash erasure, firmware upload or
authenticated Gateway batch ACK in this slice.**

After Gateway's PR #338 segmented 39-page test passed on the user's PC,
this slice exercises the **existing** `ObservationStore`, not a new
sequence allocator or a standalone mock, over the *host-only* segmented
128 KiB + 28 KiB logical Flash adapter.

Test scenario:

1. Create one real `ObservationStore` with 39 logical 4 KiB pages,
   backed by two independent fake NOR devices:
   - pages 0..31: existing Product 128 KiB,
   - pages 32..38: former History 28 KiB.
   The Geofence/Security/Config/InternalFS physical gap is never mapped.
2. Initialize the real two-control-page journal, then prepare and fill
   all 30 primary data pages using the actual OBS1 append+commit/readback
   state machine: 30 x 42 = **1260 logical observations**.
3. Prepare the next data page and append observation #1261 to logical
   page 32: the physical first page of the separate supplemental region.
   No second Tracker sequence source or reset is used.
4. Selectively mark observation #1261 released through existing
   ObservationStore machinery (host mock assumes external authenticated
   proof; it does **NOT** mint or verify a real Gateway ACK). Verify the
   older primary observation #1 remains retained, and no primary NOR
   writes were needed for this supplemental release.
5. Reboot/recover ObservationStore using the same two NOR buffers,
   verify incarnation continuity, #1261's durable release, #1's
   retention, and next sequence #1262; append #1262 in supplemental
   page 32 and reboot again to prove next #1263.
6. Refuse legacy/foreign History evidence and missing exclusive legacy
   owner proof without writing to either physical NOR.

Regression entrypoint:

`bash tools/ci/run_kq05_tracker_segmented_39page_host.sh`

The runner compiles the actual `observation_store.cpp`,
`observation_store_control.cpp` and `observation_store_format.cpp`
under ASan/UBSan. The test remains isolated behind the compile-time
`ORUN_KQ05_SEGMENTED_HOST_TEST` requirement and is also included in the
KQ05 PC LAB preflight.

### What this gate does NOT establish

- The target RAK4630 binary can actually fit the 39-page store's RAM/
  stack requirements. ObservationStore's per-page arrays grew to 39;
  physical measurement and firmware builds are separate release gates.
- Real nRF `FlashMutationGate` handles History + Product in one verified
  async/SoftDevice writer without overlapping ownership.
- Physical Gateway OCQ2 pages carry a durable **Gateway identity**
  binding or legacy migration has been approved.
- Any new selective authenticated Gateway Batch Custody ACK has
  successfully been verified by Tracker, including multi-Gateway
  duplicate/overlap cases or real LoRa duty-cycle timing.
- KQ05 fields are proven safe for a release. The P0 field gate remains
  blocked.

Even if this PC gate passes, the whole project is not field ready.
