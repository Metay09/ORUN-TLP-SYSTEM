// SF5E host stubs: just enough Arduino/nRF surface to run
// sf5e_observation_qual.cpp on the host (test_sf5e_qual_probe.cpp).
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#define F(value) value

struct TestSerial {
  std::string output;
  std::string input;
  size_t input_index = 0;
  void begin(unsigned) {}
  explicit operator bool() const { return true; }
  int available() const { return input_index < input.size(); }
  int read() {
    if (input_index >= input.size()) return -1;
    return static_cast<unsigned char>(input[input_index++]);
  }
  void print(const char* value) { output += value; }
  void println(const char* value) { print(value); output += '\n'; }
  void flush() {}
  template <typename... Args> void printf(const char* format, Args... args) {
    // Adafruit nRF52 1.7.0 Print::printf uses a 256-byte buffer.
    char buffer[256];
    const int length = snprintf(buffer, sizeof(buffer), format, args...);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(buffer));
    output += buffer;
  }
};
inline TestSerial Serial;

inline uint32_t fake_millis = 0;
inline uint32_t millis() { return ++fake_millis; }
inline void delay(uint32_t ms) { fake_millis += ms; }

struct TestFicr {
  uint32_t CODEPAGESIZE;
  uint32_t CODESIZE;
  uint32_t DEVICEID[2];
};
struct TestUicr { uint32_t NRFFW[1]; };
struct TestWdt { uint32_t RUNSTATUS; uint32_t RR[8]; };
struct AlwaysReady {
  operator uint32_t() const { return 1U; }
  AlwaysReady& operator=(uint32_t) { return *this; }
};
struct RandomByte {
  operator uint32_t() const { return static_cast<uint32_t>(rand()) & 0xFFU; }
};
struct TestRng {
  uint32_t CONFIG;
  AlwaysReady EVENTS_VALRDY;
  uint32_t TASKS_START;
  uint32_t TASKS_STOP;
  RandomByte VALUE;
};
extern TestFicr* NRF_FICR;
extern TestUicr* NRF_UICR;
extern TestWdt* NRF_WDT;
extern TestRng* NRF_RNG;
constexpr uint32_t WDT_RR_RR_Reload = 0x6E524635UL;
[[noreturn]] void NVIC_SystemReset();
