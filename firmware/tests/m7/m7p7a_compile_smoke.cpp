// M7P7A build-only smoke target.
//
// This file is never part of the production rak4630 environment, and the
// image this target produces is never flashed to hardware (see
// docs/milestones/M7P7A.md). Its only purpose is to force the pinned
// Bluefruit + InternalFS sources and ORUN's FlashMutationGate bridge into
// the same *linked* image so PlatformIO actually compiles AND links the
// patched vendor code before M7P7A can merge.
//
// Merely taking the address of a global object (e.g. `(void)&Bluefruit;`)
// is not sufficient: at this build's optimization level the compiler proves
// that expression has no observable effect and removes it entirely, so the
// linker never pulls Bluefruit52Lib/InternalFileSytem out of their archives
// and the patched translation units are silently absent from the final ELF
// even though they compiled cleanly. Actually calling begin() forces a real
// undefined-symbol reference, which is what makes the linker retain
// AdafruitBluefruit::begin() (containing the M7P7A SoC-owner handoff) and,
// transitively through bond_init()->InternalFS.begin(), the patched
// flash_nrf5x.c erase/program paths. check_storage_layout.py's
// check_exclusive_owner post-link guard then inspects the resulting ELF's
// symbol table and fails the build if either patch is missing.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>

#include "flash_mutation_gate.h"

namespace {
orun_tlp::FlashMutationGate gate;
}

void setup() {
  // Never executed: this .elf/.hex is a compile/link artifact only. Calling
  // gate.begin() forces the strong ORUN arbitration hooks from
  // flash_mutation_gate.cpp into this smoke image.
  (void)gate.begin();
  (void)InternalFS.begin();
  (void)Bluefruit.begin();
}

void loop() {}
