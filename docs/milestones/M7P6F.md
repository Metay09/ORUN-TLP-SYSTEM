# M7P6F — SecurityStore v2 + durable A2D replay persistence

Status: **FINAL TARGETED RECONCILIATION + TX-REBOOT ASSERT HOST PASS — M3 PHYSICAL INTERRUPTED-ERASE SENTINEL PASS**

Baseline: `main@d1a9720e2f2a80274e379c032cd1cc9a94b25800`
(M7P7G merged and architecture closeout current).

Branch: `feat/m7p6f-securitystore-v2-replay-state`.

## 1. Goal

M7P6F implements the persistence foundation recorded in M7P6D §11 before any
protected A2D RF/BLE application path exists:

```text
SecurityStore v1 recovery
        |
power-cut-safe v1 -> v2 migration
        |
v2 typed security-state append log
        |
TX reserve state + A2D replay reserve state
        |
internal authenticated-counter replay admission
```

The product invariant is:

> An authenticated A2D counter that is already accepted or conservatively
> burned across reset must never be accepted again for the same credential
> lifetime.

This slice deliberately adds **no production caller** of the A2D admission API.

## 2. Non-goals

M7P6F does not add or change:

- TLP v1 bytes or packet families;
- a TLP v2 wire header/envelope;
- production AES-CCM/HKDF execution;
- provisioning/commissioning;
- BLE application authorization;
- remote config writes, COMMAND/RESULT, EVENT/LOST or MESSAGE;
- RF airtime, SX1262 configuration or listen policy;
- the security flash partition boundaries;
- ConfigStore, HistoryStore or BLE bond/InternalFS formats.

The future secure receive path must still authenticate/decrypt **before** it is
allowed to call SecurityStore replay admission.

## 3. Flash ownership and v2 format

The physical allocation is unchanged:

```text
0x0E7000..0x0E8000  SecurityStore page A
0x0E8000..0x0E9000  SecurityStore page B
```

Format v1 remains read/migration-compatible and is never reinterpreted.

Format v2 keeps:

- 32-byte page header;
- 68-byte CREDENTIAL record.

The old TX-only tail is replaced by a 40-byte typed `SECURITY_STATE` append
log:

```text
offset  size  field
0       16    credential_id
16      4     key_epoch
20      1     kind
21      3     reserved = 0
24      8     value
32      4     CRC32
36      4     commit = 0
```

Authorized kinds are exactly:

```text
1 = TX_RESERVE_EXCLUSIVE_BOUND
2 = A2D_REPLAY_EXCLUSIVE_BOUND
```

There are 99 state slots per page and the final 36 bytes must remain erased.
Unknown kinds, nonzero reserved bytes, bad CRC/commit, credential/epoch
mismatch, append gaps, decreasing per-kind bounds or programmed reserved-tail
bytes fail recovery closed.

The v1 36-byte TX_RESERVE codec remains only for exact legacy recovery and
migration fixtures. New firmware writes only v2 security state.

## 4. v1 -> v2 migration

Only an authoritative, structurally valid, device-bound v1 page may migrate.

Migration is an A/B transaction:

1. recover the v1 credential and highest durable TX exclusive bound;
2. erase the inactive destination page;
3. write/read-verify the v2 header body with activation still erased;
4. carry the TX bound as a v2 TX state record when nonzero;
5. create no A2D replay state merely because migration occurs;
6. write/read-verify the credential;
7. program/read-verify page activation last;
8. only then make v2 authoritative in RAM;
9. erase the old v1 page as later maintenance.

Power loss before activation leaves v1 authoritative. Power loss after
activation leaves v2 authoritative. Failure to erase the superseded v1 page
does not roll authority back because the v2 generation is higher.

FOREIGN, UNSUPPORTED and FAULT stores never auto-migrate.

Once v2-aware firmware has recovered v1, it does not append new v1 TX records.
If migration fails in that boot, protected TX remains closed rather than
silently extending the legacy schema.

## 5. A2D replay semantics

The replay durable value is an **exclusive bound** with an initial block size
of 8.

For an already-authenticated counter `c`:

1. `c <= runtime_hwm` -> reject with no flash write;
2. `runtime_hwm < c < durable_bound` -> accept in RAM only;
3. `c >= durable_bound` -> persist/read-verify the smallest block-aligned
   exclusive bound greater than `c`, then advance the runtime HWM and report
   acceptance;
4. if no representable exclusive bound greater than `c` exists -> reject
   fail-closed with no wrap;
5. application dispatch is forbidden until an accepted result is returned.

On reboot, when durable replay bound `B != 0`, runtime HWM becomes `B - 1`.
Thus all counters below `B` are conservatively burned. Up to seven unused
counter values may be lost per reservation boundary/reboot; availability may
degrade, replay safety may not.

A persistence operation that cannot be verified reports rejection. If its
record actually reached flash before verification failed, fresh recovery sees
that durable bound and burns the corresponding range conservatively.

Replay persistence is distinct from command idempotency. A later power loss
after replay commit but before command execution can lose an operation; a
future COMMAND layer still needs its own command_id/result/idempotency design.

## 6. Compaction

Format-v2 compaction uses the same activation-last invariant:

1. erase inactive page;
2. write/verify header body;
3. write latest TX bound if present;
4. write latest A2D replay bound if present;
5. write credential;
6. activate page last;
7. switch RAM authority;
8. erase superseded page;
9. append the operation that triggered compaction, if one was pending.

The shared typed log therefore supports TX-only, A2D-only and combined
snapshots without creating fixed per-kind sub-partitions.

## 7. Concurrency and ownership

SecurityStore remains loop-owned and uses the existing
`FlashMutationGate` security ports:

- security-critical append commits block a protected TX/A2D operation;
- erase/compaction/migration maintenance uses the maintenance port;
- only one SecurityStore job exists at a time;
- unread A2D replay results cannot be silently invalidated by a credential
  replacement;
- no callback invokes flash mutation.

No new task, heap allocation, timer or background worker is introduced.

## 8. Validation matrix

Software/build validation on `605c6a67e4f8bac61023dac91ae0498bb2441046`:

- full `./firmware/tests/run_host_tests.sh`: **PASS**;
- `-Wall -Wextra -Werror`: **PASS**;
- ASan + UBSan: **PASS**;
- M7P6F v1/v2 format golden/malformed checks: **PASS**;
- M7P6F SecurityStore v2/migration/replay checks: **PASS**;
- legacy/regression host suites including RF, BLE, persistence, startup and
  watchdog guards: **PASS**;
- `pio run -d firmware -e rak4630`: **SUCCESS**;
- RAM: **22,752 / 248,832 B = 9.1%**;
- Flash: **237,848 / 815,104 B = 29.2%**.

Relative to the final M7P7G production build
(22,672 B RAM / 234,456 B flash), M7P6F adds **80 B RAM** and
**3,392 B flash**. Security partition geometry remains unchanged.

The following implementation checks are now evidenced by the passing host
suite; independent audit still must verify that the tests and invariants are
sufficient:



- v1 and v2 header/record golden + malformed codec behavior;
- exact 99-slot/36-byte-tail v2 packing;
- v1 -> v2 success with TX bound preservation and no migration-created A2D
  record;
- migration failure at every pre-activation program write leaves v1
  authoritative;
- post-activation old-page erase failure still recovers v2;
- no v1 append after migration failure;
- equal-generation committed pages fail closed;
- FOREIGN/UNSUPPORTED/FAULT never auto-migrate;
- duplicate/old replay reject with no flash mutation;
- inside-reserve replay accept with no flash mutation;
- reserve crossing, large counter jump and overflow behavior;
- program failure and failed readback/verification;
- reboot property: no counter below the durable replay bound is accepted;
- append gaps, unknown kinds, bound rollback and reserved-tail corruption;
- TX-only, A2D-only and combined v2 compaction;
- existing TX nonce non-reuse property and FlashMutationGate priority tests;
- full host suite including warnings-as-errors + ASan/UBSan — **PASS**;
- production `rak4630` build and linked RAM/flash size comparison — **PASS**;
- independent security/storage audit and resulting fix/retest cycle — **PENDING**.

A real-hardware flash/reboot sentinel will be decided after independent audit.
No physical persistence or power-cut claim is made by the host/build results.

## 9. Independent audit disposition

Independent security/storage audit against code-bearing head
`605c6a67e4f8bac61023dac91ae0498bb2441046` found no BLOCKER and no demonstrated
TX nonce reuse or A2D replay re-acceptance, but reported one HIGH, four MEDIUM and
five LOW findings.

Post-audit changes on this branch:

- **H1 fixed:** a non-empty append slot whose entire commit word remains erased is
  classified as uncommitted torn state and burned rather than turning the whole
  credential lifetime into FAULT. A partially programmed/non-erased invalid commit
  remains FAULT. Tests now inject real partial body writes and full-body/erased-commit
  cases instead of naming a clean no-write failure "torn".
- **M1 fixed:** replay admission now requires the authenticated frame's
  `credential_id` and `key_epoch` together with the counter. A retry after
  credential rotation is rejected against the new active lifetime.
- **M2 fixed:** the test flash now enforces production's erased-destination
  precondition. After a failed/ambiguous append, SecurityStore inspects the target:
  a readable dirty slot is consumed before retrying, while an unreadable target now
  closes protected service for the boot rather than guessing and creating a durable
  erased-gap ambiguity. Same-boot continuation after an isolated readable dirty slot
  remains explicitly tested.
- **M3 accepted as a bounded fail-closed availability residual:** if power is lost
  during old-page erase after the new page is already active, a partially erased
  old committed header may be indistinguishable from corruption or, when the erased
  bits alter the version byte into a future value, from an unsupported schema.
  Recovery therefore remains fail-closed as either FAULT or UNSUPPORTED rather than
  risk rollback. A true partial-erase host test records the bounded behavior. This
  residual requires a physical RAK4631 power-cut sentinel before final closure; it
  is not described as physically validated.
- **M4 fixed at the SecurityStore boundary:** any failure while page activation is
  ambiguous forces SecurityStore to `FAULT` for the rest of the boot, so RAM can
  never continue issuing protected TX under stale authority if a late activation
  physically lands. A new integration test uses the real SecurityStore and real
  FlashMutationGate timeout/quarantine/late-completion state machines together and
  verifies reboot selects the physically activated higher generation.
- **L1 fixed:** a failed automatic v1 migration cannot be opportunistically restarted
  by the A2D path in the same boot.
- **L2 documented:** every successful recovery of a provisioned credential burns the
  prior TX reserve headroom and therefore requires one fresh TX reserve before protected
  TX can resume. Repeated resets/brownouts consume shared-log slots and increase wear;
  this is the nonce-safety tradeoff, not free behavior.
- **L3 deferred/non-blocking:** the current A/B flow may erase a page that is already
  blank. Avoiding that erase safely across reboot needs explicit proof the entire page,
  not merely its header, is erased. No speculative page-state optimization is added in
  this security fix slice.
- **L4 documented:** a valid authenticated authority that deliberately jumps counters
  by at least one replay block can force one replay-state append per accepted frame.
  Unauthenticated traffic still cannot write flash. Sender-rate/counter-jump governance
  belongs to the future authenticated authority/envelope implementation.
- **L5 architecture rule fixed:** the security ADR now explicitly forbids deliberate
  reuse of retired `credential_id` or `K_root`; historical uniqueness is a future
  provisioning/authority responsibility because SecurityStore retains only the active
  credential.

The pre-audit PASS/build evidence above is historical evidence for
`605c6a67...` and is not reused as final validation for the post-audit code.

Post-audit software revalidation on
`eda1f7e499bec074a17c2f28558bbe0ea4f54aba`:

- full `./firmware/tests/run_host_tests.sh`: **PASS**;
- warnings-as-errors path: **PASS**;
- ASan + UBSan path: **PASS**;
- M7P6F v1/v2 format checks: **PASS**;
- M7P6F SecurityStore v2/migration/replay checks: **PASS**;
- new real SecurityStore + FlashMutationGate timeout/quarantine integration:
  **PASS**;
- all previously covered RF/BLE/persistence/startup/watchdog regression suites:
  **PASS**.

RAK4630 production rebuild on branch head
`3e7dc2d3ecaa9524730b2b7bd8d54201de7fe01e` (2026-09-25):

- `pio run -d firmware -e rak4630`: **SUCCESS**;
- RAM: **22,760 / 248,832 B = 9.1%**;
- Flash: **238,264 / 815,104 B = 29.2%**;
- delta versus final merged M7P7G production image
  (22,672 B RAM / 234,456 B flash): **+88 B RAM / +3,808 B flash**.

This build confirms compile/link/size only. It is not physical persistence,
power-cut, replay or flash-endurance evidence.

### Final independent reconciliation audit on `7539653`

The final independent audit returned **PASS WITH FIXES**: no BLOCKER/HIGH,
no demonstrated TX nonce reuse, and no demonstrated A2D double acceptance.
It confirmed H1/M1/M2/M4/L1 and the documented L5 boundary, while identifying
one new MEDIUM availability/endurance defect and three LOW hardening gaps:

- **N1 MEDIUM — fixed in branch, revalidation pending:** persistent
  "physically dirtied target + failure/verify mismatch" could consume append
  slots, enter compaction and repeatedly erase pages on every poll.
  SecurityStore has a boot-scoped three-failure circuit breaker for ordinary
  mutation failures. A later independent reconciliation found that one accepted
  SoftDevice operation timing out while FlashMutationGate retained quarantined
  ownership could be miscounted as several failures by subsequent loop polls.
  That case is now explicit: an unreconciled accepted async mutation causes
  immediate FAULT-until-reboot; late completion only releases the physical
  gate and cannot revive same-boot security authority. Persistent ordinary
  failures remain bounded by the three-failure breaker.
- **N2 LOW — fixed in branch, revalidation pending:** if `fail()` cannot read
  the append target, it no longer advances `state_used`. It enters FAULT for
  the boot so it cannot create an erased slot followed by a later committed
  record. Fresh reboot recovery decides from durable bytes.
- **N3 LOW — documentation/physical acceptance clarified:** partial erase may
  recover as either FAULT or UNSUPPORTED. Both are accepted fail-closed outcomes
  for the sentinel. An older generation PROVISIONED result or a lower recovered
  TX/A2D bound is never acceptable.
- **N4 LOW — fixed in branch, revalidation pending:** every record body is now
  read-verified before its commit word is programmed, including the asynchronous
  FlashMutationGate path. A mismatched body therefore remains uncommitted and
  burnable instead of becoming a committed CRC failure.
- The real SecurityStore + FlashMutationGate integration test includes an
  append-body timeout/late-SUCCESS case before the existing page-activation
  ambiguity case. It now verifies the chosen fail-closed policy: timeout of an
  accepted quarantined mutation immediately faults SecurityStore, at least five
  subsequent normal loop polls cause no extra mutation/failure activity, late
  SUCCESS releases gate ownership without reviving the store, and reboot
  recovers authoritatively from the uncommitted dirty slot.

The pre-fix host/build results above remain historical evidence only for their
recorded code heads.

Final-audit-fix host revalidation on branch head
`3a64cd6e82dd24d0656939b5d8c2fa3258f05f03` (2026-09-25):

- `./firmware/tests/run_host_tests.sh`: **PASS**;
- M7P6F SecurityStore v2/migration checks: **PASS**;
- M7P6F SecurityStore/FlashMutationGate timeout integration: **PASS**;
- all printed compatibility/RF/BLE/persistence/startup/watchdog regressions:
  **PASS**;
- the host script compiles its C++ suites with `-Wall -Wextra -Werror` and
  `-fsanitize=address,undefined`; the completed run therefore also supplies
  warnings-as-errors + ASan/UBSan revalidation for those compiled host paths.

Fresh RAK4630 production rebuild on branch head
`4a1fb7b36b51e4d0ca420a08bd7bd208bb7f14da` (2026-09-25):

- `pio run -d firmware -e rak4630`: **SUCCESS**;
- RAM: **22,768 / 248,832 B = 9.1%**;
- Flash: **238,248 / 815,104 B = 29.2%**;
- versus the pre-final-audit-fix build recorded at
  22,760 B RAM / 238,264 B flash: **+8 B RAM / -16 B flash**;
- application-ceiling and exclusive-owner guards passed during the build;
- all pinned R4/R2.1/M7P4/M7P7A dependency patches were verified/applied.

This build is historical evidence for `4a1fb7b`; code changed after
the second independent reconciliation and therefore requires a fresh host/
sanitizer run and RAK4630 build before physical testing.

Second independent reconciliation at `8a3c470` returned **PASS WITH FIXES**.
It confirmed N1 wear bounding, N2 and N4, but found one MEDIUM availability
issue: one accepted SoftDevice timeout could be counted as several failures
while the physical request remained quarantined. The selected policy is
fail-closed rather than a timed retry heuristic. FlashBackend now exposes only
the concrete "unreconciled accepted mutation" fact; FlashMutationGate security
ports report their quarantine state, and SecurityStore enters FAULT immediately
for that condition. Ordinary failures still use the three-failure breaker.
The integration test now inserts five ordinary `store.poll()` calls before
late completion and requires no synthetic failure/write/erase activity.

The M3 physical sentinel remains pending.

Second-reconciliation-fix host revalidation on
`5e97f6f0c75a1005ca3dcf90ec7f011bfde4a467` (2026-09-25):

- `./firmware/tests/run_host_tests.sh`: **PASS**;
- M7P6F SecurityStore v2/migration checks: **PASS**;
- M7P6F SecurityStore/FlashMutationGate timeout integration: **PASS**;
- all printed compatibility/RF/BLE/persistence/startup/watchdog regressions:
  **PASS**;
- the script's compiled host paths use `-Wall -Wextra -Werror` and
  `-fsanitize=address,undefined`, so warnings-as-errors + ASan/UBSan also
  passed for those paths.

Fresh RAK4630 production build after the second-reconciliation fix on
`a29db9f150ef0b3fddee7697ad30fe3d3359c029` (2026-09-25):

- `pio run -d firmware -e rak4630`: **SUCCESS**;
- RAM: **22,768 / 248,832 B = 9.1%**;
- Flash: **238,512 / 815,104 B = 29.3%**;
- delta versus the prior post-fix build at 22,768 B RAM / 238,248 B flash:
  **0 B RAM / +264 B flash**;
- exclusive-owner and application-ceiling guards passed;
- pinned R4/R2.1/M7P4/M7P7A framework/driver patches were verified/applied.

This is compile/link/size evidence only; it is not physical persistence,
brownout, partial-erase or flash-endurance evidence.

Final targeted reconciliation on the unchanged production code returned
**PASS**. It confirmed the async-timeout FAULT policy, no synthetic retries
during quarantine, late-completion ownership-only behavior, the ordinary
three-failure wear breaker, and no TX/A2D rollback path. Two non-blocking items
were noted:

- superseded old-page erase timeout is intentionally different: because the
  higher-generation page is already authoritative, `kEraseOldPage` failure
  records maintenance failure rather than immediately faulting the store. If
  late completion reconciles before another security mutation, PROVISIONED may
  continue safely. If another mutation is attempted while the erase is still
  quarantined, that attempt performs no physical write and the unreconciled
  guard faults SecurityStore;
- the real-gate reboot scenario did not explicitly assert post-timeout TX
  counter continuity. The required regression assertion is now added: after
  reboot, the next TX counter must be at or above one full reservation block
  and remain in epoch 1, proving pre-timeout counters are not reissued.

Because the TX continuity change touched only a host-test assertion and
documentation, the prior RAK4630 production build still describes the unchanged
production firmware binary.

Final host revalidation after adding the explicit reboot TX-continuity assertion
on `3a3a3ca4302062877d19d14dd128c20db3404e04` (2026-09-25):

- full `./firmware/tests/run_host_tests.sh`: **PASS**;
- M7P6F SecurityStore v2/migration checks: **PASS**;
- M7P6F SecurityStore/FlashMutationGate timeout integration: **PASS**;
- all printed compatibility/RF/BLE/persistence/startup/watchdog regressions:
  **PASS**.

### M3 physical interrupted-erase sentinel

A dedicated TEST-ONLY target,
`rak4630_m7p6f_m3_partial_erase_probe`, is added after software/audit closure.
It is intentionally isolated from the production `rak4630` image and
destructively owns only the two-page SecurityStore partition
`0x0E7000..0x0E9000` on the selected test device.

The probe uses the nRF52840's hardware `ERASEPAGEPARTIAL` primitive to create
a physically mixed superseded-page image after a fully committed,
higher-generation page already exists. It requires observed byte changes while
the stale page remains nonblank, writes a valid monotonic TX-state marker only
to the intact higher-generation page, resets, and then runs the production
SecurityStore synchronous recovery code.

Acceptance is exactly:

- higher-generation PROVISIONED with TX counter not below the marker bound and
  an old A2D counter rejected; or
- fail-closed FAULT / UNSUPPORTED.

UNPROVISIONED, FOREIGN, an older/lower-bound PROVISIONED result, TX rollback or
A2D replay re-acceptance is a failure.

This sentinel provides real nRF52840 flash partial-erase/reboot evidence. It
does **not** by itself claim that an external electrical brownout or power yank
has been reproduced.

First physical probe attempt (2026-09-25) was **INCONCLUSIVE, not a recovery
failure**. The test image uploaded and ran on a real RAK4631, but the sparse
stale-page fixture had only about 180 programmed bytes clustered at the start
of the page. NVMC partial-erase durations 5/10/20/30/40 ms changed all 180
programmed bytes to erased state (`non_ff_bytes=0`) on every attempt, so the
probe never produced the required mixed partial-page image and correctly
reported `FAIL no_mixed_partial_state_observed`. Production SecurityStore
recovery was therefore never exercised against the target physical condition.

The probe fixture is revised to fill all 99 v2 state slots on the stale page
with valid, nondecreasing repeated TX/A2D bounds, distributing programmed bits
across nearly the complete 4 KiB page without increasing stale-page authority.
The duration sweep is also tightened to 1/2/3/4/5/10/20 ms.

The revised image built successfully (RAM 8,940 B / 3.6%, flash 69,072 B /
8.5%) and its DFU ZIP passed `unzip -t`. Direct bootloader DFU reported
`Device programmed.`, but serial observation after activation was blank.
That run is also **INCONCLUSIVE**: no sentinel PASS/FAIL line was observed, so
it is not evidence about SecurityStore recovery.

The test harness itself had an observability flaw: a fresh/no-marker boot
started destructive flash work immediately after DFU and could soft-reset
before a monitor was attached. The sentinel was therefore changed to remain
idle and repeat `M7P6F M3 READY send RUN` until an operator explicitly sends
`RUN`. Production firmware remained unchanged.

The serial-armed revision built cleanly on 2026-09-26:
RAM 8,984 B / 3.6%, flash 69,440 B / 8.5%, exclusive-owner/application-ceiling
guards PASS. Direct bootloader DFU reported `Device programmed.`, the real
RAK4631 emitted the repeated READY banner, and the operator explicitly started
the run. Physical output for 1/2/3/4/5/10/20 ms partial-erase operations was
identical: `changed_bytes=4060 non_ff_bytes=0 illegal_1_to_0_bytes=0`.
Therefore every programmed byte in the dense stale page read back erased even
at 1 ms, and the old spatial-mix criterion again reported
`FAIL no_mixed_partial`.

That result is **INCONCLUSIVE for M3 recovery, not a SecurityStore failure**.
The probe assumption was wrong: the nRF52840 specification defines bits after
an incomplete partial erase as *undefined*; it does not require a spatial mix
of erased and unerased bytes. Requiring `non_ff_bytes > 0` therefore cannot
serve as a hardware-independent proof that erase was interrupted.

The physical method is revised again to target the actual M3 failure mode more
directly. After both valid generations and the monotonic TX marker are durable,
the test-only image puts NVMC into erase mode, arms the hardware WDT for about
1 ms, and immediately starts a real full-page `ERASEPAGE` of the superseded
old page. nRF52840 execution from flash stalls during erase while hardware
peripherals continue running. If the WDT resets the MCU before the erase-return
instruction can execute, the next boot must contain DOG without SREQ in the
saved reset reason; only then does the sentinel invoke production
`SecurityStore` recovery. If erase returns first, the immediate software
reset produces SREQ and the run is rejected as unproven. This is real
reset-during-NVMC-erase evidence, but still not an external electrical
brownout/power-yank test.

Physical run on 2026-09-26 reached that path on a real RAK4631:
the monitor observed the intentional USB disconnect/reconnect, reboot reported
`reset_reason=0x00000002`, and the sentinel's acceptance logic returned
`PASS outcome=PROVISIONED`. That PASS is computed only when the recovered TX
counter is at or above the durable 768 marker in epoch 1 and the old A2D
counter is rejected. The human-readable detail line itself was malformed by
the pinned embedded printf implementation's `%llu` handling
(`tx=lu epoch=768 ...`), so those shifted display fields are not used as
evidence.

Before final M3 closure, the test-only harness was hardened in two ways:
the 64-bit TX value is printed as explicit high/low 32-bit fields, and a
critical section masks USB/RTOS callbacks between the ~1 ms WDT start and the
`ERASEPAGE` write so a scheduling delay cannot create a DOG reset before
erase starts. Because the successful pre-hardening run left its 768 marker
durable in the SecurityStore partition, the hardened rerun uses a distinct
1024 marker. That prevents the freshly flashed test image from mistaking the
previous run's marker for its own interrupted-erase phase; the RUN path erases
and restages both test pages before writing the new marker.

The hardened sentinel rebuilt cleanly on 2026-09-26:
RAM 8,984 B / 3.6%, flash 69,248 B / 8.5%, with exclusive-owner and
application-ceiling guards PASS. Direct bootloader DFU reported
`Device programmed.`. On the real RAK4631, the operator observed repeated
`READY send RUN`, explicitly started the test, then observed:

```text
M7P6F M3 INTERRUPT armed marker_tx=1024 wdt_ms~1; starting old-page erase
Disconnected ...
Reconnecting ... Connected!
M7P6F M3 INTERRUPTED-ERASE SENTINEL BOOT
M7P6F M3 phase=recovery-after-interrupted-erase reset_reason=0x00000002
M7P6F M3 SENTINEL PASS outcome=PROVISIONED tx_hi=0 tx_lo=1024 epoch=1 tx_nonrollback=yes a2d_old_reject=yes
```

This is the accepted M3 physical result. The DOG reset reason proves the
hardware watchdog reset occurred on the deliberately armed erase path, the
higher-generation credential remained authoritative after reboot, the next TX
counter was exactly the durable 1024 marker in epoch 1, and the old A2D replay
counter was rejected. No rollback or replay re-acceptance was observed.
Accepted hardened-run outcome rate: PROVISIONED 1/1, FAULT/UNSUPPORTED 0/1.

M3 is therefore physically closed for the deterministic real
reset-during-NVMC-erase sentinel on the tested RAK4631. This is **not** an
external electrical brownout/power-yank test and does not claim flash-endurance
validation. Production firmware remained unchanged by all sentinel hardening.


After M3 closure, a TEST-ONLY post-sentinel cleanup helper was built and
physically run on the same development RAK4631 before restoring production
firmware. The helper requires an explicit serial `CLEAN` command, owns only
SecurityStore `0x0E7000..0x0E9000`, erases both pages through
`NrfSecurityFlash`, and read-verifies every byte in the 8 KiB region as
`0xFF`. Its build passed with RAM 8,916 B / 3.6% and flash 54,744 B / 6.7%,
including exclusive-owner/application-ceiling guards. Physical output was:

```text
M7P6F SECURITY CLEAN accepted
M7P6F SECURITY CLEAN PASS pages=2 all_ff=yes
```

This cleanup is operational test-fixture hygiene, not additional SecurityStore
runtime evidence. It deliberately does not touch ConfigStore, History,
BLE/InternalFS bonds, bootloader/settings or other flash partitions.

### Wear notes added by audit

The original §11.7 illustrative wear arithmetic did not include reset storms or all
authenticated counter-jump patterns. For the current implementation:

- each provisioned reboot burns unused TX counters and appends a fresh TX reserve;
- a reset/brownout loop can therefore accelerate shared-log consumption and compaction;
- a large but valid authenticated A2D counter jump may consume one append immediately;
- current compaction still performs conservative erase preparation even when a page may
  already be blank.

These are availability/endurance considerations. They do not weaken the replay/nonce
ordering requirement. Physical endurance remains unclaimed.

## 10. Merge gate

The M7P6F production-code validation/audit cycle is complete and the remaining
M3 physical interrupted-erase sentinel now passes on real RAK4631 hardware.
The sentinel commits after the last production build touch only TEST-ONLY probe
code and documentation; production firmware remains the binary described by
the recorded production build evidence above.

Do not reinterpret this result as flash-endurance validation or as an external
electrical brownout/power-yank test. Those remain separate physical evidence
classes.
