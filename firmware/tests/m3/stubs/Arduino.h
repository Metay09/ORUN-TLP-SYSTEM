#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

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

// SAADC: the battery divider on WB_A0. Tests set fake_analog_value and read
// back the configuration the firmware chose.
enum eAnalogReference { AR_DEFAULT, AR_INTERNAL_3_0 };
constexpr uint8_t WB_A0 = 5;
inline eAnalogReference fake_analog_reference = AR_DEFAULT;
inline int fake_analog_resolution = 10;
inline uint32_t fake_analog_value = 0;
inline unsigned fake_analog_reads = 0;
inline int fake_analog_last_pin = -1;
inline void analogReference(eAnalogReference mode) { fake_analog_reference = mode; }
inline void analogReadResolution(int bits) { fake_analog_resolution = bits; }
inline uint32_t analogRead(uint32_t pin) {
  ++fake_analog_reads;
  fake_analog_last_pin = static_cast<int>(pin);
  return fake_analog_value;
}

struct TestSerial {
  std::string output;
  std::string input;
  size_t input_index = 0;
  void begin(unsigned) {}
  int available() const { return input_index < input.size(); }
  int read() {
    if (input_index >= input.size()) return -1;
    return static_cast<unsigned char>(input[input_index++]);
  }
  void queueInput(const char* value) {
    if (value != nullptr) input += value;
  }
  void print(const char* value) { output += value; }
  void println(const char* value) { print(value); output += '\n'; }
  template <typename... Args> void printf(const char* format, Args... args) {
    // Adafruit nRF52 1.7.0 Print::printf formats into a 256-byte stack buffer
    // and then writes the UNTRUNCATED length, so a longer line prints stack
    // bytes on the device. Model that limit here: one printf call must fit.
    char buffer[256];
    const int length = snprintf(buffer, sizeof(buffer), format, args...);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(buffer));
    output += buffer;
  }
};
inline TestSerial Serial;
inline unsigned fake_idle_calls = 0;
inline void delay(uint32_t) { ++fake_idle_calls; }
