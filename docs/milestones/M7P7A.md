# M7P7A — BLE flash arbitration + SoftDevice SoC event ownership

Status: **IMPLEMENTED ON BRANCH — full host suite PASS; production RAK4630 build PASS; isolated BLE compile-link smoke PASS with linked-symbol evidence; BLE runtime remains OFF.**

Baseline: `main@9585590b64b2df7a827b89fac99470fccc526d78`
(M7P6B merged plus post-merge architecture checkpoint).

Branch: `feat/m7p7a-ble-flash-arbitration`

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
- two-file patch apply/restore returns vendor sources byte-for-byte.

Validation evidence from the canonical Debian checkout:

- `PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7a_patch_ble_flash.py`: **PASS**; installed framework pins matched.
- `bash firmware/tests/run_host_tests.sh`: **PASS** end-to-end, including M7P7A flash/event arbitration and all legacy/persistence/startup regressions.
- `pio run -d firmware -e rak4630`: **PASS**.
  - RAM: **15,436 B / 248,832 B (6.2%)**
  - Flash: **159,296 B / 815,104 B (19.5%)**
  - M7P4 relocation patch: applied/verified.
  - M7P7A arbitration/event-bridge patch: applied/verified.
  - exclusive-owner and application-ceiling post-link guards completed without error.
  - compiler warnings shown in this build are from the pinned SX126x-Arduino dependency, not newly-added ORUN source.

Compared with the M7P6B production build (15,420 B RAM / 158,128 B flash), this slice adds **16 B RAM** and **1,168 B flash** to the shipped composition.

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

**Verified post-fix (canonical Debian checkout):**

```text
pio run -d firmware -e rak4630_m7p7a_compile
RAM:   16,104 / 248,832 bytes (6.5%)
Flash: 127,916 / 815,104 bytes (15.7%)
SUCCESS
```

Flash usage in this env rose from 58,384 B (address-of only, nothing actually linked) to
127,916 B once `begin()` forced real linkage — itself corroborating evidence that a materially
larger amount of Bluefruit/InternalFS code is now present in the image.

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

Host/build PASS will not be reported as physical BLE PASS.
