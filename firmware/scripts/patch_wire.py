"""R4 pinned transform for bounded Adafruit nRF52 Wire master waits.

The framework source is patched only for the PlatformIO process and restored at
normal process exit. A backup also permits fail-closed recovery after an
interrupted build. Host tests exercise the pure transform.
"""
import atexit
import hashlib
import json
from pathlib import Path

ORIGINAL_GIT_BLOB_SHA = "c3e2df7001619d242c7f82acd447c2a7cb32a63b"
MARKER = "// ORUN R4 bounded nRF52 Wire waits\n"


def git_blob_sha(source):
    data = source.encode()
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("R4 Wire source fragment changed; dependency re-audit required")
    return source.replace(old, new, 1)


def transform_segment(source, start_marker, end_marker, replacements):
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    segment = source[start:end]
    for old, new in replacements:
        segment = replace_once(segment, old, new)
    return source[:start] + segment + source[end:]


def transform(source):
    if git_blob_sha(source) != ORIGINAL_GIT_BLOB_SHA:
        raise RuntimeError("R4 unrecognized Wire_nRF52.cpp; dependency re-audit required")

    helper = r'''
namespace {
constexpr uint32_t kOrunWireEventTimeoutMs = 25;
constexpr uint32_t kOrunWireMaxSpins = 2000000;
constexpr uint32_t kOrunWireAbortMaxSpins = 200000;
volatile bool orun_wire_timeout_flag = false;
volatile bool orun_wire_reset_required_flag = false;

void orunWireAbort(NRF_TWIM_Type* twim)
{
  orun_wire_timeout_flag = true;

  // Nordic TWIM cannot be STOPped while suspended. RESUME is harmless when
  // already active and is required before STOP for a suspended transaction.
  // Clear STOPPED first so an old event cannot masquerade as this abort.
  twim->EVENTS_STOPPED = 0x0UL;
  twim->TASKS_RESUME = 0x1UL;
  twim->TASKS_STOP = 0x1UL;

  const uint32_t started = millis();
  uint32_t spins = 0;
  while (!twim->EVENTS_STOPPED &&
         (uint32_t)(millis() - started) < 2 &&
         ++spins < kOrunWireAbortMaxSpins) {}

  // STOPPED is the hardware proof that TWIM is stopped and EasyDMA has
  // finished accessing RAM. If it never arrives, do NOT disable/re-enable the
  // peripheral or hand pins to GPIO recovery. The application must reset the
  // MCU instead of guessing that DMA is quiescent.
  if (!twim->EVENTS_STOPPED) {
    orun_wire_reset_required_flag = true;
    return;
  }

  const uint32_t errors = twim->ERRORSRC;
  twim->ERRORSRC = errors;
  twim->EVENTS_RXSTARTED = 0x0UL;
  twim->EVENTS_TXSTARTED = 0x0UL;
  twim->EVENTS_LASTRX = 0x0UL;
  twim->EVENTS_LASTTX = 0x0UL;
  twim->EVENTS_STOPPED = 0x0UL;
  twim->EVENTS_SUSPENDED = 0x0UL;
  twim->EVENTS_ERROR = 0x0UL;
  twim->ENABLE = (TWIM_ENABLE_ENABLE_Disabled << TWIM_ENABLE_ENABLE_Pos);
  twim->ENABLE = (TWIM_ENABLE_ENABLE_Enabled << TWIM_ENABLE_ENABLE_Pos);
}

bool orunWireWaitEvent(NRF_TWIM_Type* twim, volatile uint32_t* event,
                       volatile uint32_t* error)
{
  const uint32_t started = millis();
  uint32_t spins = 0;
  while (*event == 0 && (error == nullptr || *error == 0)) {
    if ((uint32_t)(millis() - started) >= kOrunWireEventTimeoutMs ||
        ++spins >= kOrunWireMaxSpins) {
      orunWireAbort(twim);
      return false;
    }
  }
  return true;
}
} // namespace

extern "C" bool orunWireTakeTimeoutFlag(void)
{
  const bool value = orun_wire_timeout_flag;
  orun_wire_timeout_flag = false;
  return value;
}

extern "C" bool orunWireTakeResetRequiredFlag(void)
{
  const bool value = orun_wire_reset_required_flag;
  orun_wire_reset_required_flag = false;
  return value;
}
'''
    source = MARKER + source
    source = replace_once(
        source,
        '#include <Adafruit_TinyUSB.h> // for Serial\n',
        '#include <Adafruit_TinyUSB.h> // for Serial\n' + helper)

    request_start = "uint8_t TwoWire::requestFrom(uint8_t address, size_t quantity, bool stopBit)\n{"
    request_end = "uint8_t TwoWire::requestFrom(uint8_t address, size_t quantity)\n{"
    source = transform_segment(source, request_start, request_end, [
        (request_start, request_start + "\n  if (orun_wire_timeout_flag) return 0;"),
        ("  while(!_p_twim->EVENTS_RXSTARTED && !_p_twim->EVENTS_ERROR);",
         "  if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_RXSTARTED, &_p_twim->EVENTS_ERROR)) return 0;"),
        ("  while(!_p_twim->EVENTS_LASTRX && !_p_twim->EVENTS_ERROR);",
         "  if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_LASTRX, &_p_twim->EVENTS_ERROR)) return 0;"),
        ("    while(!_p_twim->EVENTS_STOPPED);",
         "    if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_STOPPED, nullptr)) return 0;"),
        ("    while(!_p_twim->EVENTS_SUSPENDED);",
         "    if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_SUSPENDED, nullptr)) return 0;"),
    ])

    tx_start = "uint8_t TwoWire::endTransmission(bool stopBit)\n{"
    tx_end = "uint8_t TwoWire::endTransmission()\n{"
    source = transform_segment(source, tx_start, tx_end, [
        (tx_start, tx_start + "\n  if (orun_wire_timeout_flag) return 4;"),
        ("  while(!_p_twim->EVENTS_TXSTARTED && !_p_twim->EVENTS_ERROR);",
         "  if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_TXSTARTED, &_p_twim->EVENTS_ERROR)) return 4;"),
        ("    while(!_p_twim->EVENTS_LASTTX && !_p_twim->EVENTS_ERROR);",
         "    if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_LASTTX, &_p_twim->EVENTS_ERROR)) return 4;"),
        ("    while(!_p_twim->EVENTS_STOPPED);",
         "    if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_STOPPED, nullptr)) return 4;"),
        ("    while(!_p_twim->EVENTS_SUSPENDED);",
         "    if (!orunWireWaitEvent(_p_twim, &_p_twim->EVENTS_SUSPENDED, nullptr)) return 4;"),
    ])
    return source


def apply(core):
    if json.loads((core / "package.json").read_text())["version"] != "1.10700.0":
        raise RuntimeError("R4 requires Adafruit nRF52 1.7.0 re-audit")

    target = core / "libraries/Wire/Wire_nRF52.cpp"
    backup = core / "libraries/Wire/Wire_nRF52.cpp.orun-original"
    source = target.read_text()

    if source.startswith(MARKER):
        if not backup.exists():
            raise RuntimeError("R4 patched Wire source found without original backup")
        original = backup.read_text()
        if source != transform(original):
            raise RuntimeError("R4 patched Wire source altered; dependency re-audit required")
    else:
        original = source
        patched = transform(original)  # Validate exact upstream before writing.
        if backup.exists() and backup.read_text() != original:
            raise RuntimeError("R4 Wire backup does not match audited upstream")
        if not backup.exists():
            backup.write_text(original)
        target.write_text(patched)

    def restore():
        try:
            if target.exists() and target.read_text().startswith(MARKER):
                target.write_text(original)
        except Exception:
            # Build result remains authoritative; the next invocation validates
            # marker + backup and fails closed if restoration was interrupted.
            pass

    atexit.register(restore)
    print("R4: verified/applied bounded Adafruit nRF52 1.7.0 Wire patch")


if "Import" in globals():
    Import("env")
    apply(Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")))
