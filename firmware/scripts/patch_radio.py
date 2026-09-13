"""Pinned source transform: fail closed, idempotent, applied before compilation.

PlatformIO post extra-script runs after dependency resolution, before builders
execute. The pure transform is also exercised by host guard tests.
"""
import hashlib
import json
from pathlib import Path

ORIGINAL_SHA256 = "38a53d01efea2b83453b4c30c3a932aa969c71735c8b09840e5fa2361298dd2e"
MARKER = "// ORUN R2.1 pinned driver serialization\n"


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("R2.1 source fragment changed; dependency re-audit required")
    return source.replace(old, new, 1)


def transform(source):
    if hashlib.sha256(source.encode()).hexdigest() != ORIGINAL_SHA256:
        raise RuntimeError("R2.1 unrecognized radio.cpp; dependency re-audit required")
    source = MARKER + '''#include "radio_driver_gate.h"
#include <atomic>
#if !defined(NRF52_SERIES)
#error ORUN driver gate requires the audited nRF52 target
#endif
static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "DIO ISR flag must be lock-free");
''' + source
    source = replace_once(source, "bool IrqFired = false;", "std::atomic<bool> IrqFired{false};")
    # No FreeRTOS software timer is armed by the supported P2P Send path.
    # Deadline runs on the application monotonic clock under the same gate.
    source = replace_once(source,
        "\tTimerSetValue(&TxTimeoutTimer, TxTimeout);\n\tTimerStart(&TxTimeoutTimer);",
        "\t// ORUN: owner supplies the P2P software TX deadline.")
    for name in ("RadioRx", "RadioRxBoosted"):
        source = replace_once(source, f"void {name}(uint32_t timeout)\n{{",
                              f"void {name}(uint32_t timeout)\n{{\n\tconfigASSERT(timeout == 0);")
    start = source.index("void RadioOnTxTimeoutIrq(void)\n{")
    end = source.index("void RadioEnforceLowDRopt", start)
    source = source[:start] + '''// Unsupported timer-based entry points cannot touch the P2P driver.
// Radio.Init still creates inactive timer handles because TimerStop uses them.
void RadioOnTxTimeoutIrq(void) {}
void RadioOnRxTimeoutIrq(void) {}

void orunRadioTimeoutLocked()
{
    TimerTxTimeout = true;
    orunRadioDispatchLocked();
    RadioStandby();
    RadioSleep(); // Includes all BUSY waits and final delay under caller's gate.
}

void orunRadioQuiesceLocked()
{
    RadioStandby();
    SX126xSetDioIrqParams(IRQ_RADIO_NONE, IRQ_RADIO_NONE,
                        IRQ_RADIO_NONE, IRQ_RADIO_NONE);
    SX126xClearIrqStatus(IRQ_RADIO_ALL);
    IrqFired.store(false);
    TimerTxTimeout = false;
    TimerRxTimeout = false;
    // A pending semaphore/late GPIO ISR is harmless: it can only dispatch
    // cleared registers or a newly armed session, never retained old bytes.
}

''' + source[end:]
    source = replace_once(source, "void RadioBgIrqProcess(void)\n{",
        '''void RadioBgIrqProcess(void)
{
    orun_tlp::radio_driver::Guard gate(true);
    orunRadioDispatchLocked();
}

void orunRadioDispatchLocked()
{''')
    source = replace_once(source,
        "\tif (IrqFired == true)\n\t{\n\t\tBoardDisableIrq();\n\t\tIrqFired = false;\n\t\tBoardEnableIrq();",
        "\tif (IrqFired.exchange(false))\n\t{")
    # The unused deep-sleep entry must also enter the gate before state writes.
    source = replace_once(source, "void RadioIrqProcessAfterDeepSleep(void)\n{",
        "void RadioIrqProcessAfterDeepSleep(void)\n{\n\torun_tlp::radio_driver::Guard gate(true);")
    source = replace_once(source,
        "\tIrqFired = true;\n\tBoardEnableIrq();\n\tRadioBgIrqProcess();",
        "\tIrqFired = true;\n\tBoardEnableIrq();\n\torunRadioDispatchLocked();")
    return source


def apply(library, core):
    if json.loads((library / "library.json").read_text())["version"] != "2.0.32":
        raise RuntimeError("R2.1 requires SX126x-Arduino 2.0.32 re-audit")
    if json.loads((core / "package.json").read_text())["version"] != "1.10700.0":
        raise RuntimeError("R2.1 requires Adafruit nRF52 1.7.0 re-audit")
    target = library / "src/radio/sx126x/radio.cpp"
    original = library / "src/radio/sx126x/radio.cpp.orun-original"
    source = target.read_text()
    if source.startswith(MARKER):
        if not original.exists() or source != transform(original.read_text()):
            raise RuntimeError("R2.1 patched source altered; clean dependency and re-audit")
    else:
        patched = transform(source)  # Validate before writing anything.
        original.write_text(source)
        target.write_text(patched)
    print("R2.1: verified/applied SX126x 2.0.32 driver gate patch")


if "Import" in globals():
    Import("env")
    apply(Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "SX126x-Arduino",
          Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")))
