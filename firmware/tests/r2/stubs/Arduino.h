#pragma once

#include <stdint.h>
#include "FreeRTOS.h"

#define F(value) value

struct TestSerial {
  void println(const char*) {}
  template <typename... Args>
  void printf(const char*, Args...) {}
};

inline TestSerial Serial;
