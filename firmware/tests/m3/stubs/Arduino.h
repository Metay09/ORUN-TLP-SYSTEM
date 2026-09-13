#pragma once
#include <stdint.h>

#define F(value) value
constexpr int OUTPUT = 1, INPUT_PULLUP = 2, HIGH = 1, LOW = 0;
constexpr uint8_t WB_IO2 = 34;
constexpr uint8_t PIN_WIRE_SDA = 13;
constexpr uint8_t PIN_WIRE_SCL = 14;

inline int sensor_power = LOW;
inline unsigned power_writes = 0;
inline int pin_modes[64]{};
inline int pin_levels[64]{};
inline bool fake_scl_stuck_low = false;
inline bool fake_sda_stuck_low = false;
inline int fake_sda_release_after_clocks = -1;
inline unsigned fake_scl_clock_pulses = 0;
inline uint64_t fake_delay_us = 0;

inline void pinMode(int pin, int mode) {
  if (pin >= 0 && pin < 64) {
    if (pin == PIN_WIRE_SCL && pin_modes[pin] == OUTPUT &&
        pin_levels[pin] == LOW && mode == INPUT_PULLUP) {
      ++fake_scl_clock_pulses;
    }
    pin_modes[pin] = mode;
  }
}

inline void digitalWrite(int pin, int level) {
  if (pin >= 0 && pin < 64) pin_levels[pin] = level;
  if (pin == WB_IO2) {
    sensor_power = level;
    ++power_writes;
  }
}

inline int digitalRead(int pin) {
  if (pin == PIN_WIRE_SCL && fake_scl_stuck_low) return LOW;
  if (pin == PIN_WIRE_SDA && fake_sda_stuck_low) {
    if (fake_sda_release_after_clocks >= 0 &&
        fake_scl_clock_pulses >=
            static_cast<unsigned>(fake_sda_release_after_clocks))
      return HIGH;
    return LOW;
  }
  if (pin >= 0 && pin < 64 && pin_modes[pin] == OUTPUT)
    return pin_levels[pin];
  return HIGH;
}

inline void delayMicroseconds(uint32_t us) { fake_delay_us += us; }

struct TestSerial {
  void println(const char*) {}
  template <typename... Args> void printf(const char*, Args...) {}
};
inline TestSerial Serial;
