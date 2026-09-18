"""M7P7A pure-transform/lifecycle tests for patch_ble_flash.py."""
import importlib.util
import json
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "patch_ble_flash.py"

FLASH_FIXTURE = r'''#include "flash_nrf5x.h"
#include "delay.h"

#define MAX_RETRY 20
static SemaphoreHandle_t _sem = NULL;
static uint32_t _flash_op_result = NRF_EVT_FLASH_OPERATION_SUCCESS;

void flash_nrf5x_event_cb (uint32_t event) {
  if ( _sem ) {
    _flash_op_result = event;
    xSemaphoreGive(_sem);
  }
}

static uint32_t wait_for_async_flash_op_completion(void) {
  return NRF_SUCCESS;
}

static bool fal_erase (uint32_t addr)
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

// helper for fal_program()
static bool fal_sub_program(uint32_t dst, void const * src, uint32_t len) {
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
'''

BLUEFRUIT_FIXTURE = r'''extern "C"
{
void flash_nrf5x_event_cb (uint32_t event) ATTR_WEAK;
}

bool AdafruitBluefruit::begin(uint8_t prph_count, uint8_t central_count)
{
  xTaskCreate( adafruit_soc_task, "SOC", CFG_SOC_TASK_STACKSIZE, NULL, TASK_PRIO_HIGH, &soc_task_hdl);

  NVIC_EnableIRQ(SD_EVT_IRQn); // enable SD interrupt

  // Create Timer for led advertising blinky
  return true;
}

void adafruit_soc_task(void* arg)
{
  switch (soc_evt)
  {
    case NRF_EVT_FLASH_OPERATION_SUCCESS:
    case NRF_EVT_FLASH_OPERATION_ERROR:
      LOG_LV1("SOC", "NRF_EVT_FLASH_OPERATION_%s", soc_evt == NRF_EVT_FLASH_OPERATION_SUCCESS ? "SUCCESS" : "ERROR");
      if ( flash_nrf5x_event_cb ) flash_nrf5x_event_cb(soc_evt);
    break;
  }
}
'''


def load_raw():
    spec = importlib.util.spec_from_file_location("patch_ble_flash", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_fixture_module():
    mod = load_raw()
    mod.FLASH_ORIGINAL_GIT_BLOB_SHA = mod.git_blob_sha(FLASH_FIXTURE)
    mod.BLUEFRUIT_ORIGINAL_GIT_BLOB_SHA = mod.git_blob_sha(BLUEFRUIT_FIXTURE)
    return mod


def expect_raises(fn, *args):
    try:
        fn(*args)
    except RuntimeError:
        return
    raise AssertionError(f"expected RuntimeError from {fn.__name__}")


def fake_core(root, flash_text=FLASH_FIXTURE, bluefruit_text=BLUEFRUIT_FIXTURE):
    root = Path(root)
    flash = root / "libraries/InternalFileSytem/src/flash/flash_nrf5x.c"
    blue = root / "libraries/Bluefruit52Lib/src/bluefruit.cpp"
    flash.parent.mkdir(parents=True, exist_ok=True)
    blue.parent.mkdir(parents=True, exist_ok=True)
    flash.write_text(flash_text)
    blue.write_text(bluefruit_text)
    (root / "package.json").write_text(json.dumps({"version": "1.10700.0"}))
    return root


def main():
    mod = load_fixture_module()

    patched_flash = mod.transform_flash(FLASH_FIXTURE)
    assert patched_flash.startswith(mod.FLASH_MARKER)
    assert mod.flash_patch_ok(patched_flash)
    assert "orun_flash_internalfs_owns && !orun_flash_internalfs_owns()" in patched_flash
    assert "if (!orun_flash_internalfs_acquire()) return false;" in patched_flash
    assert patched_flash.count("orun_flash_internalfs_release_if_present();") >= 4

    patched_blue = mod.transform_bluefruit(BLUEFRUIT_FIXTURE)
    assert patched_blue.startswith(mod.BLUEFRUIT_MARKER)
    assert mod.bluefruit_patch_ok(patched_blue)
    assert "orun_flash_gate_set_bluefruit_soc_owner(true)" in patched_blue
    assert "orun_flash_gate_soc_event_cb(soc_evt)" in patched_blue
    assert patched_blue.index("orun_flash_gate_soc_event_cb(soc_evt)") < \
        patched_blue.index("flash_nrf5x_event_cb(soc_evt)")

    # Exact-pin and exact-fragment fail-closed behavior.
    expect_raises(mod.transform_flash, FLASH_FIXTURE + "\n// drift")
    expect_raises(mod.transform_bluefruit, BLUEFRUIT_FIXTURE + "\n// drift")
    expect_raises(mod.replace_once, "x", "missing", "new", "fixture")

    # Two-file apply validates and patches together; restore returns both
    # framework files byte-for-byte to their originals.
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp)
        restore = mod.apply(core)
        paths = mod.target_paths(core)
        assert paths["flash"][0].read_text() == patched_flash
        assert paths["bluefruit"][0].read_text() == patched_blue
        restore()
        assert paths["flash"][0].read_text() == FLASH_FIXTURE
        assert paths["bluefruit"][0].read_text() == BLUEFRUIT_FIXTURE

    # Patched file without its pinned backup is never trusted.
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp, patched_flash, patched_blue)
        expect_raises(mod.apply, core)

    # Bonus check against the real installed PlatformIO framework if present.
    fresh = load_raw()
    core = Path.home() / ".platformio/packages/framework-arduinoadafruitnrf52"
    flash = core / fresh.FLASH_RELATIVE_PATH
    blue = core / fresh.BLUEFRUIT_RELATIVE_PATH
    if flash.exists() and blue.exists():
        assert fresh.git_blob_sha(flash.read_text()) == fresh.FLASH_ORIGINAL_GIT_BLOB_SHA
        assert fresh.git_blob_sha(blue.read_text()) == fresh.BLUEFRUIT_ORIGINAL_GIT_BLOB_SHA
        print("M7P7A bonus check: installed framework pins match")
    else:
        print("M7P7A bonus check: installed framework absent; skipped")

    print("M7P7A BLE framework patch checks: PASS")


if __name__ == "__main__":
    main()
