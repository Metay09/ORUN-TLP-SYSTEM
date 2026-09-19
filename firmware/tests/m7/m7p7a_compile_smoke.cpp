// M7P7A build-only smoke target.
//
// This file is never part of the production rak4630 environment. Its only
// purpose is to force the pinned Bluefruit + InternalFS sources and ORUN's
// FlashMutationGate bridge into the same linked image so PlatformIO actually
// compiles the patched vendor code before M7P7A can merge.
#include <Arduino.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>

#include "flash_mutation_gate.h"

namespace {
orun_tlp::FlashMutationGate gate;
}

void setup() {
  // Taking the addresses forces the framework objects' defining translation
  // units into the link. Calling gate.begin() likewise forces the strong ORUN
  // arbitration hooks from flash_mutation_gate.cpp into this smoke image.
  (void)&Bluefruit;
  (void)&InternalFS;
  (void)gate.begin();
}

void loop() {}
