# M7P6F — SecurityStore v2 + durable A2D replay persistence

Status: **INDEPENDENT AUDIT FOUND FIXES — H1/M1/M2/M4 CODE FIXES APPLIED; M3 RESIDUAL DOCUMENTED; REVALIDATION PENDING**

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
  precondition. After a failed/ambiguous append, SecurityStore inspects the target
  and consumes a dirty or unreadable slot before retrying; same-boot A2D continuation
  is explicitly tested.
- **M3 accepted as a bounded fail-closed availability residual:** if power is lost
  during old-page erase after the new page is already active, a partially erased
  old committed header may be indistinguishable from corruption. Recovery remains
  FAULT rather than risk rollback. A true partial-erase host test now records this
  behavior. This residual requires a physical RAK4631 power-cut sentinel before
  final closure; it is not described as physically validated.
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
`605c6a67...`. Because production security code and tests changed after audit, it must
not be reused as final validation for the current head. Full host/sanitizer and RAK4630
build must be rerun after these fixes, followed by audit reconciliation.

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

Do not merge until the validation matrix above is reconciled against actual
test output and independent review.

In particular, host PASS is not a physical-flash endurance/power-cut PASS, and
build SUCCESS is not evidence for actual persistence behavior on RAK4631.
