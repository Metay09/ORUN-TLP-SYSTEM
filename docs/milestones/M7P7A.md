# M7P7A — BLE flash arbitration + SoftDevice SoC event ownership

Status: **MERGED TO MAIN — full host suite PASS; production RAK4630 build PASS; isolated BLE compile-link smoke PASS with linked-symbol evidence; the review-found ownership-transfer invariant violation (§4.1) is fixed and covered by host tests; BLE runtime remains OFF and no physical BLE PASS is claimed.**

Baseline: `main@9585590b64b2df7a827b89fac99470fccc526d78`
(M7P6B merged plus post-merge architecture checkpoint).

Branch: `feat/m7p7a-ble-flash-arbitration`

PR: `#20`

Merged to `main` at: `e012a76b01f21b9575840d25a5a26ad78721021d`

## 1. Purpose

M7P3-M7P6 established one cooperative `FlashMutationGate` for History,
Config and Security. M7P4 relocated Adafruit `InternalFS`/BLE bond storage
into `0x0EB000..0x0ED000`, but deliberately left two blockers before
`Bluefruit.begin()` could be enabled safely:

1. Bluefruit's SoC task and `FlashMutationGate::pumpEvents()` would both call
   `sd_evt_get()`, racing on Nordic's single global SoC event queue.
2. Adafruit `flash_nrf5x.c` called `sd_flash_write`/
   `sd_flash_page_erase` directly, bypassing ORUN's physical flash ownership.

This slice closes only those prerequisites. It does **not** enable BLE,
advertising, pairing, provisioning, GATT services or DFU.

## 2. Ownership model

A two-value physical owner token is added beside `FlashMutationGate`:

- ORUN gate (History / Config / Security);
- InternalFS bond storage.

The token is a fixed 32-bit atomic; no heap or scheduler/framework is added.

When SoftDevice is enabled:

- an admitted ORUN gate operation acquires the token and retains it across
  `NRF_ERROR_BUSY` retries until completion/failure;
- patched InternalFS acquires the same token before each Nordic erase/program
  primitive and releases it only after its own completion event;
- therefore both paths cannot submit physical flash mutations concurrently.

The existing internal ORUN admission order remains unchanged:

`SEC_CRITICAL > History > Config > SEC_MAINT`.

InternalFS is not turned into a new persistent/config owner and does not gain
access to History/Config/Security pages. Its filesystem remains confined to the
M7P4 bond partition.

## 3. SoC event ownership

Before Bluefruit starts, `FlashMutationGate::pumpEvents()` retains the proven
M7P3-M7P6 behavior and directly drains `sd_evt_get()`.

The pinned M7P7A Bluefruit patch changes the future BLE-active path:

1. after Bluefruit's SoC task exists, but before `SD_EVT_IRQn` is enabled,
   Bluefruit declares itself the sole SoC-event queue owner;
2. `FlashMutationGate::pumpEvents()` then stops calling `sd_evt_get()`;
3. Bluefruit forwards flash completion events through one weak C hook;
4. only gate-owned events enter a one-word atomic mailbox;
5. the cooperative loop consumes that mailbox and updates the existing gate
   state machine.

Important ordering: the Bluefruit SoC task forwards the event to ORUN **before**
calling `flash_nrf5x_event_cb()`. The latter may wake the blocked InternalFS
task, which can release its owner token immediately. Forward-first preserves
unambiguous event ownership.

## 4. InternalFS stale-semaphore protection

The patched `flash_nrf5x_event_cb()` signals InternalFS only while InternalFS
owns the shared token.

Without this filter, an ORUN-gate flash completion could leave a stale semaphore
token in the stock driver. A later bond write could then consume that stale token
and return before its own physical flash operation completed.

Outside ORUN the hooks are weak; absent a strong ORUN bridge, vendor behavior
remains stock.

## 4.1 Ownership-transfer invariant and independent timeout budgets (post-review fix)

An independent code-review pass on this branch found a real M7P7A blocker in
`FlashMutationGate` itself (`firmware/src/flash_mutation_gate.cpp`), not in the
vendor patch: the original bounded application-level timeout
(`kOperationTimeoutMs`, 4000ms) released the shared physical-flash token
(`releaseSlot()` → `releaseSharedFlash(SharedFlashOwner::kGate)`) on **any**
timeout, including one that fired *after* SoftDevice had already accepted an
`sd_flash_write`/`sd_flash_page_erase` call for that request
(`Slot::submission_accepted == true`) but before its completion event arrived.

**Exact failure mode audited:** if that early release happened, InternalFS
could immediately `orun_flash_internalfs_try_acquire()` the freed token and
begin its own Nordic flash call while ORUN's earlier operation was still
genuinely in flight. The stale completion event for ORUN's abandoned
operation, once it finally arrived, would be forwarded by the pinned
`bluefruit.cpp` SoC task to *both* `orun_flash_gate_soc_event_cb()` (a no-op,
since it checks *current* shared-owner identity and the owner had already
changed to InternalFS) and the patched `flash_nrf5x_event_cb()` — which
signals InternalFS's semaphore filtered only by *current* owner, not by which
physical request the event actually belongs to. If that stale signal arrived
before InternalFS's own subsequent `sd_flash_write`/`sd_flash_page_erase` call
had itself completed, `wait_for_async_flash_op_completion()`'s semaphore take
would consume the stale give and return immediately — letting a bond write
report success before its own physical Nordic operation actually finished.

**A second, related defect** (the review's original finding): the same
`kOperationTimeoutMs` (4000ms) budget, measured from admission, was also used
while an admitted request was merely *waiting to acquire* the shared token
from InternalFS — a phase entirely separate from, and unrelated in direction
to, `patch_ble_flash.py`'s `ORUN_FLASH_ARBITER_WAIT_MS` (4500ms), which bounds
the mirror-image wait: how long InternalFS's patched driver waits to *acquire*
this same token while ORUN owns it. Reusing one shared 4000ms clock for both
"wait to acquire" and "time since acquired" meant a queued ORUN request could
be failed closed before it ever had one opportunity to call
`sd_flash_write`/`sd_flash_page_erase` at all — the fix is to give that phase
its own independent budget, not to compare it against InternalFS's unrelated
4500ms figure (see the corrected note under "Fix" below).

**Required invariant (now enforced):** once SoftDevice has accepted a physical
flash operation for a client, the shared token must not be transferred to
InternalFS until that exact operation has a definitive completion event, or
the request enters a fail-closed/quarantined state that cannot misattribute a
late event.

**Fix — two independent timeout clocks, not a constant bump:**

- `kTokenWaitTimeoutMs` (6000ms, new): bounds only how long an admitted
  request waits to *acquire* the shared token while InternalFS owns it. This
  is an independent, explicit ORUN-side bounded-liveness policy, **not** a
  safety relationship with InternalFS's own `ORUN_FLASH_ARBITER_WAIT_MS`
  (4500ms). The two constants each bound one side's own "wait to acquire"
  phase in opposite directions; they do not bound the same phase, and
  `kTokenWaitTimeoutMs > ORUN_FLASH_ARBITER_WAIT_MS` is not, and was never
  meant to be, a proof that ORUN is guaranteed to obtain the token before this
  fires. In particular, `ORUN_FLASH_ARBITER_WAIT_MS` does not bound how long
  InternalFS then *holds* the token after acquiring it: once its own
  `sd_flash_write`/`sd_flash_page_erase` call is accepted, the stock
  (unpatched) `wait_for_async_flash_op_completion()` blocks on
  `xSemaphoreTake(_sem, portMAX_DELAY)` — an unbounded wait for the real
  completion event, with no software timeout of its own. If this timeout
  fires, the ORUN request fails closed **without releasing InternalFS's
  token** — `attemptSubmit()`'s phase-1 branch only runs while InternalFS
  owns the token, so ORUN's own `releaseSlot()` call there is a harmless
  no-op on the shared owner (its compare-exchange expects the current owner
  to already be `kGate`, which it is not). Nothing was ever submitted to
  SoftDevice by ORUN in this branch (`submission_accepted` is still false),
  so this can never create ownership ambiguity, regardless of *why*
  InternalFS still owned it. A
  permanently stuck InternalFS/SoftDevice path (the `portMAX_DELAY` case truly
  never completing) is a real liveness gap this constant does not solve; it
  remains a physical BLE runtime concern for later validation (see §8), not an
  ownership-safety one, and the accepted-operation quarantine invariant above
  is unaffected by it either way.
- `kOperationTimeoutMs` (4000ms, unchanged value, **redefined start point**):
  now reset the instant the token is actually acquired
  (`Slot::token_acquired`), not at admission — giving the post-acquisition
  phase (SD `BUSY` retries on the submission call itself, then waiting for the
  completion event) its own fresh budget, independent of how long phase 1
  took. This is the same 4-second physical-operation figure the ADR already
  documents elsewhere (SEC_CRITICAL aging, etc.); it was not increased.
- If this second timeout fires with `submission_accepted == false` (still
  retrying `BUSY` on the submission call itself), nothing was submitted, so
  `releaseSlot()` still applies unchanged (no quarantine).
- If it fires with `submission_accepted == true` and no completion event yet,
  the slot is **quarantined** (`Slot::quarantined`, new;
  `FlashMutationGate::quarantineSlot()`): the caller is told `kFailed` so the
  application never wedges, but `kind`/`target`/`admitted`/
  `submission_accepted`, `in_flight_owner_` and the shared `kGate` token are
  left exactly as they are. A new submission from the same owner is rejected
  (`kind != kNone`) until the quarantine resolves; other owners simply stay
  queued, exactly as they already do behind any in-flight operation.
- `FlashMutationGate::handleFlashEvent()` reconciles a quarantined slot when
  the real, definitive completion finally arrives: it releases the token via
  the normal `releaseSlot()` path and records a new diagnostic counter,
  `Diagnostics::late_completions`, instead of `completions_success`/
  `completions_error` — the caller already observed one outcome (`kFailed`)
  for that logical request and must not see a second, contradictory one.

No change to `patch_ble_flash.py`'s vendor transform, `ORUN_FLASH_ARBITER_WAIT_MS`
(4500ms, unchanged), or the SoC-event forwarding/ordering in §3 — the fix is
entirely inside `FlashMutationGate`'s own state machine, the only place that
was violating the invariant. This does not weaken any existing guard, change
production behavior for the SoftDevice-disabled path (unaffected; the sync
backends are untouched), or enable BLE runtime.

Host coverage added directly for this fix (`firmware/tests/m7/test_m7p7a_flash_gate.cpp`,
scenarios 4-8):

1. InternalFS holds the token past the old 4000ms budget (up to 4200ms): the
   queued ORUN request must not falsely time out before getting a submission
   opportunity, and must still submit once InternalFS releases. A second case
   confirms the new `kTokenWaitTimeoutMs` (6000ms) is itself still bounded --
   InternalFS holding forever does eventually fail closed, cleanly, with no
   quarantine (nothing was ever submitted).
2. SoftDevice accepts the write (`submission_accepted`), then the completion
   is delayed past `kOperationTimeoutMs`: the caller is failed closed, but
   `orun_flash_internalfs_try_acquire()` must keep failing and
   `orun_flash_internalfs_owns()` must stay false -- InternalFS cannot acquire
   ownership while the physical completion is unresolved. A same-owner
   resubmission attempt is also rejected, not silently allowed to start a
   second physical operation.
3. The late completion, once delivered, is reconciled as this exact quarantined
   request's own event: `late_completions` increments; `completions_success`/
   `completions_error` do not (never double-counted; never misattributed).
4. After reconciliation, both InternalFS and a fresh ORUN request can acquire
   the token again and complete normally.
5. A submission that never gets past `NRF_ERROR_BUSY` (never accepted at all)
   still times out on the unchanged `kOperationTimeoutMs` budget and releases
   the token cleanly -- confirming the pre-acceptance path is intentionally
   exempt from quarantine.

A pre-existing M7P3 host test (`test_m7p3_flash_gate.cpp`) asserted the *old*
(incorrect) behavior -- that an accepted-but-unconfirmed operation's late event
became `spurious_events` and the slot was immediately reusable after a bare
timeout. That assertion was updated to require `late_completions` and to
confirm the slot stays rejected (`kFailed` on resubmission) until the real
event is reconciled, matching the corrected invariant.

## 5. Framework pinning and build guards

New pinned patch:

`firmware/scripts/patch_ble_flash.py`

Audited Adafruit nRF52 framework version:

`1.10700.0` / upstream 1.7.0.

Pinned Git blob SHAs:

- `flash_nrf5x.c`: `5ee7302595568ba75dfb7b89e101328d0890c5d9`
- `bluefruit.cpp`: `9a91dc5b86d91360a8d3b9df9514f4ba3717abf0`

The patch is temporary/restored at PlatformIO process exit, matching the existing
Wire/radio/InternalFS patch discipline.

`check_storage_layout.py` now fails the linked image if:

- InternalFS is linked without the exact M7P4 relocation **and** M7P7A flash
  arbitration patch; or
- Bluefruit is linked without the M7P7A SoC-event bridge.

The application ceiling and partition geometry are unchanged.

## 6. Compatibility / product impact

No intentional change to:

- TLP v1 bytes or golden fixtures;
- RF frequency/SF/BW, airtime or relay behavior;
- GNSS/tracking interval;
- role compatibility;
- History/Config/Security on-flash formats;
- flash partition addresses;
- tracker sleep/listen policy;
- current shipped BLE behavior (still OFF).

This is a prerequisite plumbing slice only.

## 7. Tests added

Focused host coverage:

- InternalFS token excludes an ORUN gate submission until release;
- ORUN submission proceeds after InternalFS release;
- once Bluefruit declares SoC ownership, `pumpEvents()` no longer drains
  `sd_evt_get()`;
- Bluefruit-forwarded gate completion finishes the correct request;
- an InternalFS-owned completion cannot become a stale gate completion;
- pinned framework transforms fail closed on source drift;
- two-file patch apply/restore returns vendor sources byte-for-byte;
- (§4.1 post-review fix) InternalFS holding the token past the old shared
  budget does not falsely time out a queued ORUN request, and the wider
  token-wait budget is itself still bounded;
- (§4.1) an accepted-but-unconfirmed ORUN operation quarantines the shared
  token on timeout instead of releasing it, and InternalFS cannot acquire
  ownership while that operation's completion is unresolved;
- (§4.1) a late completion for a quarantined request is reconciled as ORUN's
  own event (`late_completions`), never double-counted as a fresh success/
  error and never left for InternalFS to consume;
- (§4.1) reconciliation makes the token available again for both InternalFS
  and a fresh ORUN request;
- (§4.1) a submission that never clears SoftDevice `BUSY` (never accepted)
  still times out and releases the token cleanly, without quarantine.

Validation evidence from the canonical Debian checkout (re-run after the §4.1 fix):

- `PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7a_patch_ble_flash.py`: **PASS**; installed framework pins matched.
- `bash firmware/tests/run_host_tests.sh`: **PASS** end-to-end (53/53), including M7P7A flash/event arbitration (8 scenarios), the updated M7P3 async-contract quarantine assertion, and all other legacy/persistence/startup regressions.
- `pio run -d firmware -e rak4630`: **PASS**.
  - RAM: **15,460 B / 248,832 B (6.2%)**
  - Flash: **159,024 B / 815,104 B (19.5%)**
  - M7P4 relocation patch: applied/verified.
  - M7P7A arbitration/event-bridge patch: applied/verified.
  - exclusive-owner and application-ceiling post-link guards completed without error.
  - compiler warnings shown in this build are from the pinned SX126x-Arduino dependency, not newly-added ORUN source.

Compared with the pre-§4.1-fix build on this same branch (15,436 B RAM / 159,296 B flash), the quarantine/two-clock fix to `FlashMutationGate` (production-linked on every build, SoftDevice-enabled or not) changes production composition by **+24 B RAM / -272 B flash** -- the two new `Slot` booleans (`token_acquired`, `quarantined`) and the `late_completions` counter cost a little RAM; the flash delta is compiler code-layout variance from the restructured branches, not a new feature surface. Compared with the M7P6B production build (15,420 B RAM / 158,128 B flash), this slice as a whole now adds **40 B RAM** and **896 B flash** to the shipped composition.

Important validation boundary: because BLE remains OFF in the production environment, Bluefruit/InternalFS are not pulled into that linked image. Therefore the production build proves the ORUN-side bridge and patch/build guards but does **not** by itself compile the transformed Bluefruit/InternalFS code. An isolated `rak4630_m7p7a_compile` build target is included specifically to close that compile/link evidence gap before merge.

### 7.1 Compile/link smoke target — fixed and closed

The initial `rak4630_m7p7a_compile` target (added in `c032a56`) failed for a test-environment reason,
not a firmware defect: as an `extends = env:rak4630` section, it inherited production's full
`extra_scripts` list even after its `lib_deps` was emptied to keep unrelated SX126x/GNSS
dependencies out of the synthetic Dependency Graph. `patch_radio.py`'s `apply()` unconditionally
resolves `SX126x-Arduino/library.json` from `$PROJECT_LIBDEPS_DIR/$PIOENV`, which this
env intentionally never installs, so every build failed with `FileNotFoundError` before reaching
compilation.

**Fix — explicit minimal `extra_scripts`, not production inheritance.** PlatformIO's `extends`
lets a child `[env:]` section redefine an option to fully replace (not merge) the parent's value —
already the mechanism this target uses for `lib_deps = ` (empty). The same replace-not-merge
behavior was verified directly (`pio run` output showing the Dependency Graph contains only
TinyUSB/InternalFS/Bluefruit both before and after) and applied to `extra_scripts`:

```ini
extra_scripts =
    pre:scripts/check_storage_layout.py
    post:scripts/patch_internalfs.py
    post:scripts/patch_ble_flash.py
```

`patch_wire.py` is dropped because `Wire_nRF52.cpp` is not part of this target's translation-unit
graph (the smoke source never includes `Wire.h`) — it patches a file that would never be compiled
here regardless. `patch_radio.py` is dropped for the same reason (`radio.cpp` is not compiled here)
and because it is the one script whose `apply()` hard-depends on `SX126x-Arduino` being installed in
*this* env's libdeps directory, which this env deliberately does not do. `check_storage_layout.py`
(pre-build audit + `check_exclusive_owner`/`check_application_ceiling` post-link guards) and both
M7P4/M7P7A framework patches are kept unchanged, since proving those patches actually apply and
that the resulting linked image is InternalFS/Bluefruit-ownership-clean is exactly this target's
purpose. `patch_radio.py` itself was not modified — it was not made tolerant of a missing
`SX126x-Arduino`, per the task constraint; the fix is that this unrelated target no longer invokes
it.

A second, independent gap was found and fixed after the `extra_scripts` fix unblocked compilation:
`flash_mutation_gate.cpp` calls `orun_tlp::monotonic::nowMs()`, defined in `monotonic_time.cpp`,
which was missing from the target's `build_src_filter`. This caused a link-time
`undefined reference to orun_tlp::monotonic::nowMs()` after all framework sources compiled
successfully. Adding `+<monotonic_time.cpp>` resolved it.

**Third gap — a successful link that proved nothing.** With both of the above fixed, the target
linked and reported `SUCCESS`, but inspecting the resulting `.elf` with `arm-none-eabi-nm` showed
**zero** occurrences of `Bluefruit` or `InternalFS` anywhere in the symbol table (639 symbols total,
consistent with a TinyUSB-only image). The smoke source's original forcing idiom,
`(void)&Bluefruit; (void)&InternalFS;`, computes an address and immediately discards it with no
observable effect; at this build's optimization level the compiler proved the statements had no
side effects and removed them entirely, so the linker never had a reason to pull
`Bluefruit52Lib`/`InternalFileSytem` out of their archives. `check_exclusive_owner`'s `bluefruit_linked`/
`internalfs_linked` regex checks against the linked `.elf` were therefore never actually exercised —
the build's "success" did not constitute the link evidence this milestone requires. This matches the
task brief's explicit warning: a successful build is not sufficient if the transformed translation
units are not actually compiled/linked.

**Fix:** `firmware/tests/m7/m7p7a_compile_smoke.cpp` now calls `InternalFS.begin()` and
`Bluefruit.begin()` directly in `setup()` (never executed — this `.elf`/`.hex` is a link-evidence
artifact only and is never flashed to hardware; see §8). A real call forces a genuine undefined-symbol
reference, which the linker must resolve by pulling the defining object files from their archives and
retaining them (and everything transitively reachable from them, including the M7P7A patched code
paths) under `--gc-sections`. This is a change to a build-only, non-shipped test target; it does not
touch `env:rak4630`, does not call `Bluefruit.begin()` from any code path reachable in production, and
does not enable BLE runtime anywhere.

**Verified post-fix (canonical Debian checkout; re-run after the §4.1 `FlashMutationGate` fix,
which also relinks into this target since `flash_mutation_gate.cpp` is one of its sources):**

```text
pio run -d firmware -e rak4630_m7p7a_compile
RAM:   16,128 / 248,832 bytes (6.5%)
Flash: 127,468 / 815,104 bytes (15.6%)
SUCCESS
```

Flash usage in this env rose from 58,384 B (address-of only, nothing actually linked) to
over 127,000 B once `begin()` forced real linkage — itself corroborating evidence that a
materially larger amount of Bluefruit/InternalFS code is now present in the image. The small
RAM/flash shift versus the first post-fix measurement (16,104 B / 127,916 B) matches the §4.1
`FlashMutationGate` delta already reported in §7's production evidence, not a change to this
target's own configuration.

`check_exclusive_owner` and `check_application_ceiling` both ran as post-link actions and completed
without raising — i.e. the M7P4/M7P7A patch-presence checks for `InternalFS linked` and
`Bluefruit linked` were this time actually evaluated (not skipped) against the real symbol table, and
passed.

Direct `arm-none-eabi-nm` inspection of `.pio/build/rak4630_m7p7a_compile/firmware.elf` confirms:

| Symbol | Type | Meaning |
| --- | --- | --- |
| `Bluefruit` | `B` (defined, bss) | Global `AdafruitBluefruit` instance linked |
| `InternalFS` | `B` (defined, bss) | Global `InternalFileSystem` instance linked |
| `_ZN17AdafruitBluefruit5beginEhh` | `T` (defined) | Patched `AdafruitBluefruit::begin()`, containing the M7P7A SoC-owner handoff line, linked |
| `orun_flash_internalfs_try_acquire` | `T` (defined) | ORUN strong hook (flash_mutation_gate.cpp), resolves the flash_nrf5x.c weak reference |
| `orun_flash_internalfs_release` | `T` (defined) | ORUN strong hook, same as above |
| `orun_flash_internalfs_owns` | `T` (defined) | ORUN strong hook, same as above |
| `orun_flash_gate_soc_event_cb` | `T` (defined) | ORUN strong hook, resolves the bluefruit.cpp weak reference |
| `orun_flash_gate_set_bluefruit_soc_owner` | `T` (defined) | ORUN strong hook, same as above |
| `flash_nrf5x_erase` / `flash_nrf5x_write` / `flash_nrf5x_flush` / `flash_nrf5x_event_cb` | `T` (defined) | Patched InternalFS driver entry points linked |
| `sd_flash_write` / `sd_flash_page_erase` | `t` (defined, local) | Nordic primitives present (both History/Config/Security's `NrfHistoryFlash`-family backends and the patched InternalFS driver call these) |

All symbols are **defined** (`T`/`t`/`B`), not undefined (`U`) — i.e. actually present in the final
linked image, not merely referenced. This closes the compile/link evidence gap noted in §6/§7 above:
the patched Bluefruit `bluefruit.cpp` and InternalFS `flash_nrf5x.c` translation units, and ORUN's
strong arbitration hooks, are proven present together in one linked RAK4630/nRF52840 image.

This remains build-only evidence. It does not execute on hardware, does not prove SoftDevice-enabled
runtime behavior, and this `.elf`/`.hex` is never flashed (§8).

## 8. Physical-validation boundary

No new physical behavior is claimed by this branch yet. The `rak4630_m7p7a_compile`
target's `.elf`/`.hex`/`.zip` build artifacts are compile/link evidence only; per
this milestone's explicit instruction they are not uploaded to hardware, and
`setup()` calling `Bluefruit.begin()`/`InternalFS.begin()` in that build-only
target (§7.1) is never executed anywhere.

Even after host/build PASS, the following remain separate M7P7 runtime/physical
work:

- actual `Bluefruit.begin()` on RAK4631;
- real bond creation and persistence in relocated InternalFS;
- simultaneous History/Config/Security activity while BLE is active;
- tracker advertising/no-client timeout behavior;
- connect/disconnect/reconnect behavior;
- BLE authorization/provisioning;
- DFU preservation and bootloader authenticity/rollback behavior.

**Known limitation carried forward from §4.1, matching an already-acknowledged
ADR gap** (`ADR_M7_PERSISTENCE_LAYOUT.md` §9: "a bounded timeout fallback for
an event that never arrives (SoftDevice fault) is a required design element
for the implementation slice that builds this gate... not decided here"):
quarantine has no secondary timeout of its own. If a completion event were
permanently lost (a genuine SoftDevice fault, not the normal case this slice
targets), the quarantined client's slot stays held until a reset, rather than
being recovered automatically. This is the intentionally conservative,
fail-closed side of the required invariant -- correctness (never misattribute
a late event) over liveness in that specific fault case -- and is explicitly
not resolved by this fix. It remains physically unverified, like the rest of
this section, and is not a regression: the pre-fix code had no correct
recovery for this case either, since it required a leaked completion race
that could itself corrupt data (§4.1).

A distinct, InternalFS-side liveness gap exists independently of ORUN's
quarantine: the stock (unpatched) `wait_for_async_flash_op_completion()`
blocks on `xSemaphoreTake(_sem, portMAX_DELAY)` with no software timeout of
its own, so if InternalFS's own accepted SoftDevice operation never
completes, InternalFS holds the shared token forever too. `kTokenWaitTimeoutMs`
(§4.1) does not and cannot fix this -- it only bounds how long *ORUN* waits to
acquire the token, and an ORUN request timing out there fails closed without
touching InternalFS's ownership at all (§4.1). Recovering from either stuck
case is the same class of problem: a genuine SoftDevice fault that this
prerequisite slice does not attempt to solve, and that real BLE runtime
validation (physical reset/watchdog behavior under an actual stuck flash
operation) must characterize later, not this host/build-only evidence.

Host/build PASS will not be reported as physical BLE PASS.
