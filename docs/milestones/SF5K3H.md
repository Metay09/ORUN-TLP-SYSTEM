# SF5K3H / 1 — real FlashBackend v3 commit-last A/B transaction engine

**State: DRAFT. New SF5K3H host tests and RAK4630 build NOT YET RUN.**

## New production-source code

- `firmware/include/security_v3_page_writer.h`
- `firmware/src/security_v3_page_writer.cpp`
- `firmware/tests/sf5/test_sf5k3h_security_v3_page_writer.cpp`

This implementation **really calls `FlashBackend::erasePage`,
`program`, `pollPending`, `read` and
`hasUnreconciledMutation`** in a bounded non-blocking state machine.
It is deliberately not called by production boot, the radio, BLE,
flash service setup, or `SecurityStore`. Therefore no target-device
physical mutation happens unless a future reviewed owner explicitly
instantiates and invokes it.

## Allowed transaction

The writer is an inert **same-credential migration / compaction
mechanism**, NOT a policy mutation/provisioning API. It can move a
fully verified **v2→v3** or **v3→v3** snapshot to the opposite A/B page
only when the immutable proposed v3 snapshot matches the physically
selected source's credential scope, TX/A2D exclusive bounds and
four Gateway entries exactly, including tombstones/revocation high
water. It reads the source credential from flash and requires an
identical device identity, root, credential ID and epoch. The version
3 output generation is computed as `selected_generation + 1`,
never accepted as a caller-supplied generation.

The source is selected via SF5K3G's fail-closed real
`security_v3_ab_preflight::inspect()`; ambiguous pages/read faults/
foreign pages and an unsupported version reject before the first
erase. The writer **never erases the source page** and cannot
create new Gateway grants, revoke credentials, rotate K_root,
advance counters, modify custody policy or release observations.

## Physical step order

1. Hold an externally acquired **exclusive, initialized, quiescent**
   security flash owner token (same constraints as FlashMutationGate).
   Inspect A/B and validate source ID/root/snapshot BEFORE mutation.
2. Erase ONLY the inactive target page. Read all 4096 bytes in
   bounded 68-byte chunks and require 0xFF.
3. Program/verify first 28 bytes of v3 header. Activation
   marker (last four bytes) remains 0xFFFFFFFF.
4. Program/verify 64-byte credential body, then program/verify its
   four-byte CRC commit marker separately.
5. Program each of five mandatory full policy snapshot entries,
   then optional TX and A2D bound records. EACH 56-byte record uses
   52-byte body readback followed by four-byte commit and full
   record verification. No other entries or tail are written.
6. Re-read complete target credential/header and stream-recover
   all 71 v3 journal slots + 20-byte erased tail; compare recovered
   state EXACTLY against preserved source. Header activation is still
   erased at this point.
7. Program only the four-byte header activation marker and verify
   full header. A newly committed physical page is NOT an
   authorization callback or a `SecurityStore::kProvisioned` state.

Asynchronous `kPending` always reconciles via `pollPending()`;
never reissue the same physical operation. Any failure/readback
mismatch/unreconciled mutation becomes terminal `kFault`, and
no later `poll` mutates flash.

## Tests / gaps

The SF5K3H host tests execute this real production engine through
a fake NOR FlashBackend (erased destination enforcement, injected
cut points, and simulated asynchronous completions), covering
v2→v3, v3→v3, tombstone/credential/counter refusal, byte cuts,
source preservation, corrupt readback and pre-activation tamper.
It additionally models a completed final activation whose asynchronous
SoftDevice result is reported as failed: the transaction stays FAULT
although reboot may legitimately inspect a physically committed v3
page. Never convert such a result into same-boot authority.

These tests do NOT prove actual SoftDevice timing, FlashMutationGate
arbitration, device resets, wear endurance or safe on-device release.
Before runtime activation, additional real `SecurityStore` state
integration and durable replay counter tests are required.

**CRITICAL:** The existing `SecurityStore::begin/recover/poll` is
UNCHANGED and its new page writer remains v2. If test code writes a
committed v3 page and then boots the current SecurityStore, the
existing downgrade guard will correctly report UNSUPPORTED and
protected service must remain disabled. No firmware upload
or Gateway custody ACK/release is authorized by this PR.

## Verification

```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3a_host.sh
bash firmware/tests/run_host_tests.sh
cd firmware
pio run -e rak4630
```

**Do not claim PASS until actual terminal evidence exists.**
Keep this PR stacked and draft. Follow-up SF5K3H phases:
(1) strengthen hard power-cut / partial-erase qualification of
this engine and (2) only then integrate authority into
`SecurityStore` with native flash ownership, fail-closed recovery
and authenticated policy-owner admission.
