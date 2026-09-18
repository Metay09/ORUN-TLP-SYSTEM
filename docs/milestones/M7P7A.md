# M7P7A — BLE flash arbitration + SoftDevice SoC event ownership

Status: **IMPLEMENTED ON BRANCH — host/build validation pending; BLE runtime remains OFF.**

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

Validation evidence is intentionally left pending until run from the canonical
Debian checkout.

## 8. Physical-validation boundary

No new physical behavior is claimed by this branch yet.

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
