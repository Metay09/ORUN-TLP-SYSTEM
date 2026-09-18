"""M7P7A pinned framework patch for BLE flash arbitration and SoC event ownership.

This patch does NOT enable BLE. It prepares the already-pinned Adafruit nRF52
1.7.0 framework so a later Bluefruit.begin() can coexist with ORUN's
FlashMutationGate:

1. InternalFS/flash_nrf5x acquires the ORUN physical-flash token before each
   Nordic erase/write and releases it only after completion.
2. InternalFS ignores flash completion events that belong to the ORUN gate,
   preventing stale semaphore tokens.
3. Bluefruit's SoC task remains the sole sd_evt_get() consumer once active and
   forwards flash completions to the ORUN bridge through weak C hooks.

All transforms are pinned to exact upstream Git blob SHAs and restored at normal
PlatformIO process exit, following patch_internalfs.py / patch_wire.py.
"""
import atexit
import hashlib
import json
from pathlib import Path

FRAMEWORK_VERSION = "1.10700.0"

FLASH_RELATIVE_PATH = "libraries/InternalFileSytem/src/flash/flash_nrf5x.c"
BLUEFRUIT_RELATIVE_PATH = "libraries/Bluefruit52Lib/src/bluefruit.cpp"

FLASH_ORIGINAL_GIT_BLOB_SHA = "5ee7302595568ba75dfb7b89e101328d0890c5d9"
BLUEFRUIT_ORIGINAL_GIT_BLOB_SHA = "9a91dc5b86d91360a8d3b9df9514f4ba3717abf0"

FLASH_MARKER = "/* ORUN M7P7A BLE flash arbitration patch */\n"
BLUEFRUIT_MARKER = "// ORUN M7P7A Bluefruit SoC event bridge patch\n"

ARBITER_WAIT_MS = 4500


def git_blob_sha(source):
    data = source.encode()
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


def replace_once(source, old, new, label):
    if source.count(old) != 1:
        raise RuntimeError(f"M7P7A {label} fragment changed; dependency re-audit required")
    return source.replace(old, new, 1)


FLASH_HOOK_INSERT = """static uint32_t _flash_op_result = NRF_EVT_FLASH_OPERATION_SUCCESS;
"""

FLASH_HOOK_BLOCK = f"""static uint32_t _flash_op_result = NRF_EVT_FLASH_OPERATION_SUCCESS;

// M7P7A: weak hooks preserve upstream behavior outside ORUN while allowing
// ORUN's FlashMutationGate to serialize InternalFS with History/Config/Security.
bool orun_flash_internalfs_try_acquire(void) __attribute__((weak));
void orun_flash_internalfs_release(void) __attribute__((weak));
bool orun_flash_internalfs_owns(void) __attribute__((weak));

#define ORUN_FLASH_ARBITER_WAIT_MS {ARBITER_WAIT_MS}

static bool orun_flash_internalfs_acquire(void) {{
  if (!orun_flash_internalfs_try_acquire) return true;
  for (uint32_t waited = 0; waited < ORUN_FLASH_ARBITER_WAIT_MS; ++waited) {{
    if (orun_flash_internalfs_try_acquire()) return true;
    delay(1);
  }}
  return false;
}}

static void orun_flash_internalfs_release_if_present(void) {{
  if (orun_flash_internalfs_release) orun_flash_internalfs_release();
}}
"""

FLASH_EVENT_OLD = """void flash_nrf5x_event_cb (uint32_t event) {
  if ( _sem ) {
"""
FLASH_EVENT_NEW = """void flash_nrf5x_event_cb (uint32_t event) {
  // Ignore ORUN-gate completions. Without this filter a gate-owned flash
  // event could leave a stale token in InternalFS's semaphore and make a
  // later bond write return before its own flash operation completed.
  if (orun_flash_internalfs_owns && !orun_flash_internalfs_owns()) return;

  if ( _sem ) {
"""

FLASH_ERASE_OLD = """static bool fal_erase (uint32_t addr)
{
  // Init semaphore for first call
  if ( _sem == NULL ) {
    _sem = xSemaphoreCreateBinary();
    VERIFY(_sem);
  }

  // Erase the page: Multiple attempts if needed
  for (uint8_t attempt = 0; attempt < MAX_RETRY; ++attempt) {
    if (NRF_SUCCESS == sd_flash_page_erase(addr / FLASH_NRF52_PAGE_SIZE)) {
      if (NRF_SUCCESS == wait_for_async_flash_op_completion()) {
        return true;
      }
    }
    delay(1);
  }
  return false;
}
"""

FLASH_ERASE_NEW = """static bool fal_erase (uint32_t addr)
{
  // Init semaphore for first call
  if ( _sem == NULL ) {
    _sem = xSemaphoreCreateBinary();
    VERIFY(_sem);
  }

  // ORUN arbitration wait is separate from the vendor's own SVC retry loop:
  // if another ORUN flash operation owns the token, wait boundedly in this
  // Bluefruit/FS task while the cooperative application loop continues.
  if (!orun_flash_internalfs_acquire()) return false;

  // Erase the page: Multiple attempts if needed.
  for (uint8_t attempt = 0; attempt < MAX_RETRY; ++attempt) {
    if (NRF_SUCCESS == sd_flash_page_erase(addr / FLASH_NRF52_PAGE_SIZE)) {
      if (NRF_SUCCESS == wait_for_async_flash_op_completion()) {
        orun_flash_internalfs_release_if_present();
        return true;
      }
    }
    delay(1);
  }

  orun_flash_internalfs_release_if_present();
  return false;
}
"""

FLASH_PROGRAM_OLD = """static bool fal_sub_program(uint32_t dst, void const * src, uint32_t len) {
  for (uint8_t attempt = 0; attempt < MAX_RETRY; ++attempt) {
    if (NRF_SUCCESS == sd_flash_write((uint32_t*) dst, (uint32_t const *) src, len/4)) {
      if (NRF_SUCCESS == wait_for_async_flash_op_completion()) {
        return true;
      }
    }
    delay(1);
  }
  return false;
}
"""

FLASH_PROGRAM_NEW = """static bool fal_sub_program(uint32_t dst, void const * src, uint32_t len) {
  if (!orun_flash_internalfs_acquire()) return false;

  for (uint8_t attempt = 0; attempt < MAX_RETRY; ++attempt) {
    if (NRF_SUCCESS == sd_flash_write((uint32_t*) dst, (uint32_t const *) src, len/4)) {
      if (NRF_SUCCESS == wait_for_async_flash_op_completion()) {
        orun_flash_internalfs_release_if_present();
        return true;
      }
    }
    delay(1);
  }

  orun_flash_internalfs_release_if_present();
  return false;
}
"""

BLUEFRUIT_EXTERN_OLD = """extern "C"
{
void flash_nrf5x_event_cb (uint32_t event) ATTR_WEAK;
}
"""
BLUEFRUIT_EXTERN_NEW = """extern "C"
{
void flash_nrf5x_event_cb (uint32_t event) ATTR_WEAK;
void orun_flash_gate_soc_event_cb(uint32_t event) ATTR_WEAK;
void orun_flash_gate_set_bluefruit_soc_owner(bool active) ATTR_WEAK;
}
"""

BLUEFRUIT_OWNER_OLD = """  NVIC_EnableIRQ(SD_EVT_IRQn); // enable SD interrupt

  // Create Timer for led advertising blinky
"""
BLUEFRUIT_OWNER_NEW = """  // M7P7A: from this point Bluefruit's SoC task is the single global
  // sd_evt_get() consumer. Tell ORUN before IRQ delivery can begin.
  if (orun_flash_gate_set_bluefruit_soc_owner) {
    orun_flash_gate_set_bluefruit_soc_owner(true);
  }
  NVIC_EnableIRQ(SD_EVT_IRQn); // enable SD interrupt

  // Create Timer for led advertising blinky
"""

BLUEFRUIT_FLASH_EVENT_OLD = """              if ( flash_nrf5x_event_cb ) flash_nrf5x_event_cb(soc_evt);
"""
BLUEFRUIT_FLASH_EVENT_NEW = """              // Route to ORUN before waking InternalFS. If InternalFS owns
              // this event, its semaphore callback may unblock another task
              // which releases the owner token immediately; ORUN must inspect
              // the owner while it still unambiguously identifies this event.
              if ( orun_flash_gate_soc_event_cb ) orun_flash_gate_soc_event_cb(soc_evt);
              if ( flash_nrf5x_event_cb ) flash_nrf5x_event_cb(soc_evt);
"""


def transform_flash(source):
    if git_blob_sha(source) != FLASH_ORIGINAL_GIT_BLOB_SHA:
        raise RuntimeError("M7P7A unrecognized flash_nrf5x.c; dependency re-audit required")
    source = FLASH_MARKER + source
    source = replace_once(source, FLASH_HOOK_INSERT, FLASH_HOOK_BLOCK, "flash hook insertion")
    source = replace_once(source, FLASH_EVENT_OLD, FLASH_EVENT_NEW, "flash event callback")
    source = replace_once(source, FLASH_ERASE_OLD, FLASH_ERASE_NEW, "flash erase path")
    source = replace_once(source, FLASH_PROGRAM_OLD, FLASH_PROGRAM_NEW, "flash program path")
    return source


def transform_bluefruit(source):
    if git_blob_sha(source) != BLUEFRUIT_ORIGINAL_GIT_BLOB_SHA:
        raise RuntimeError("M7P7A unrecognized bluefruit.cpp; dependency re-audit required")
    source = BLUEFRUIT_MARKER + source
    source = replace_once(source, BLUEFRUIT_EXTERN_OLD, BLUEFRUIT_EXTERN_NEW, "Bluefruit weak hooks")
    source = replace_once(source, BLUEFRUIT_OWNER_OLD, BLUEFRUIT_OWNER_NEW, "Bluefruit SoC owner handoff")
    source = replace_once(
        source, BLUEFRUIT_FLASH_EVENT_OLD, BLUEFRUIT_FLASH_EVENT_NEW,
        "Bluefruit flash event forwarding")
    return source


def flash_patch_ok(source):
    return (
        source.startswith(FLASH_MARKER)
        and "orun_flash_internalfs_try_acquire" in source
        and "orun_flash_internalfs_owns" in source
        and "ORUN_FLASH_ARBITER_WAIT_MS" in source
        and source.count("orun_flash_internalfs_acquire()") >= 2
        and source.count("orun_flash_internalfs_release_if_present();") >= 4
    )


def bluefruit_patch_ok(source):
    return (
        source.startswith(BLUEFRUIT_MARKER)
        and "orun_flash_gate_soc_event_cb" in source
        and "orun_flash_gate_set_bluefruit_soc_owner" in source
        and "orun_flash_gate_set_bluefruit_soc_owner(true)" in source
        and "orun_flash_gate_soc_event_cb(soc_evt)" in source
    )


def target_paths(core):
    flash = core / FLASH_RELATIVE_PATH
    bluefruit = core / BLUEFRUIT_RELATIVE_PATH
    return {
        "flash": (flash, Path(str(flash) + ".orun-m7p7a-original")),
        "bluefruit": (bluefruit, Path(str(bluefruit) + ".orun-m7p7a-original")),
    }


def _prepare(target, backup, marker, transform):
    source = target.read_text()
    if source.startswith(marker):
        if not backup.exists():
            raise RuntimeError(f"M7P7A patched {target.name} found without original backup")
        original = backup.read_text()
        patched = transform(original)
        if source != patched:
            raise RuntimeError(f"M7P7A patched {target.name} altered; dependency re-audit required")
        return original, patched

    original = source
    patched = transform(original)
    if backup.exists() and backup.read_text() != original:
        raise RuntimeError(f"M7P7A {target.name} backup does not match audited upstream")
    return original, patched


def apply(core):
    if json.loads((core / "package.json").read_text())["version"] != FRAMEWORK_VERSION:
        raise RuntimeError("M7P7A requires Adafruit nRF52 1.7.0 re-audit")

    paths = target_paths(core)
    prepared = {}
    prepared["flash"] = _prepare(*paths["flash"], FLASH_MARKER, transform_flash)
    prepared["bluefruit"] = _prepare(
        *paths["bluefruit"], BLUEFRUIT_MARKER, transform_bluefruit)

    # Both files are fully validated before either is changed.
    for key in ("flash", "bluefruit"):
        target, backup = paths[key]
        original, patched = prepared[key]
        if not backup.exists():
            backup.write_text(original)
        target.write_text(patched)

    def restore():
        for key, marker in (("flash", FLASH_MARKER), ("bluefruit", BLUEFRUIT_MARKER)):
            target, _ = paths[key]
            original, _patched = prepared[key]
            try:
                if target.exists() and target.read_text().startswith(marker):
                    target.write_text(original)
            except Exception:
                # Next build revalidates marker + backup and fails closed.
                pass

    print("M7P7A: verified/applied BLE flash arbitration + SoC event bridge patch")
    return restore


if "Import" in globals():
    Import("env")
    core = Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52"))
    atexit.register(apply(core))
