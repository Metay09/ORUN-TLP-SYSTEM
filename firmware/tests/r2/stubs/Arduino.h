#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <type_traits>
#include "FreeRTOS.h"

#define F(value) value

struct TestSerial {
  std::string output;
  void println(const char* text) { output += text; output += '\n'; }
  template <typename... Args>
  void printf(const char* format, Args... args) {
    // Host libc supports long long; reject it here so host success cannot hide
    // the target nano printf limitation, even in compiled but unexecuted paths.
    static_assert(((!std::is_same_v<Args, unsigned long long> &&
                    !std::is_same_v<Args, long long>) && ...),
                  "Serial diagnostics must not depend on long-long printf");
    char buffer[256];
    const int length = snprintf(buffer, sizeof(buffer), format, args...);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(buffer));
    output += buffer;
  }
};

inline TestSerial Serial;
