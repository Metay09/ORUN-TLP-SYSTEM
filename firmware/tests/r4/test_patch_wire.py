#!/usr/bin/env python3
import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).parents[2] / "scripts" / "patch_wire.py"
spec = importlib.util.spec_from_file_location("patch_wire", SCRIPT)
patch_wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch_wire)

FIXTURE = '''#include <Adafruit_TinyUSB.h> // for Serial
uint8_t TwoWire::requestFrom(uint8_t address, size_t quantity, bool stopBit)
{
  while(!_p_twim->EVENTS_RXSTARTED && !_p_twim->EVENTS_ERROR);
  while(!_p_twim->EVENTS_LASTRX && !_p_twim->EVENTS_ERROR);
  if (stopBit || _p_twim->EVENTS_ERROR)
  {
    _p_twim->TASKS_STOP = 0x1UL;
    while(!_p_twim->EVENTS_STOPPED);
  }
  else
  {
    _p_twim->TASKS_SUSPEND = 0x1UL;
    while(!_p_twim->EVENTS_SUSPENDED);
  }
}
uint8_t TwoWire::requestFrom(uint8_t address, size_t quantity)
{
  return requestFrom(address, quantity, true);
}
uint8_t TwoWire::endTransmission(bool stopBit)
{
  while(!_p_twim->EVENTS_TXSTARTED && !_p_twim->EVENTS_ERROR);
  if (txBuffer.available()) {
    while(!_p_twim->EVENTS_LASTTX && !_p_twim->EVENTS_ERROR);
  }
  if (stopBit || _p_twim->EVENTS_ERROR)
  {
    _p_twim->TASKS_STOP = 0x1UL;
    while(!_p_twim->EVENTS_STOPPED);
  }
  else
  {
    _p_twim->TASKS_SUSPEND = 0x1UL;
    while(!_p_twim->EVENTS_SUSPENDED);
  }
}
uint8_t TwoWire::endTransmission()
{
  return endTransmission(true);
}
'''

patch_wire.ORIGINAL_GIT_BLOB_SHA = patch_wire.git_blob_sha(FIXTURE)
patched = patch_wire.transform(FIXTURE)
assert patched.startswith(patch_wire.MARKER)
assert "orunWireTakeTimeoutFlag" in patched
assert "kOrunWireEventTimeoutMs = 25" in patched
assert "kOrunWireMaxSpins" in patched
for unbounded in (
    "while(!_p_twim->EVENTS_RXSTARTED",
    "while(!_p_twim->EVENTS_LASTRX",
    "while(!_p_twim->EVENTS_TXSTARTED",
    "while(!_p_twim->EVENTS_LASTTX",
    "while(!_p_twim->EVENTS_STOPPED);",
    "while(!_p_twim->EVENTS_SUSPENDED);",
):
    assert unbounded not in patched
assert "return 0;" in patched
assert "return 4;" in patched

try:
    patch_wire.transform(FIXTURE + "// changed\n")
    raise AssertionError("changed upstream source must fail closed")
except RuntimeError:
    pass

print("R4 pinned Wire timeout transform guards: PASS")
