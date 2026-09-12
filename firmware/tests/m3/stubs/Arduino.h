#pragma once
#include <stdint.h>

#define F(value) value
constexpr int OUTPUT = 1, HIGH = 1, LOW = 0, WB_IO2 = 34;
inline int sensor_power = LOW;
inline unsigned power_writes = 0;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int level) { sensor_power = level; ++power_writes; }
struct TestSerial {
  void println(const char*) {}
  template <typename... Args> void printf(const char*, Args...) {}
};
inline TestSerial Serial;
