#pragma once
#include <stddef.h>
#include <stdint.h>

struct TwoWire {
  bool status_ok = true;
  uint16_t bytes_available = 0;
  uint8_t register_pointer = 0;
  uint8_t read_index = 0;

  void begin() {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t value) {
    register_pointer = value;
    return 1;
  }
  uint8_t endTransmission(bool = true) { return status_ok ? 0 : 4; }
  uint8_t requestFrom(uint8_t, uint8_t quantity) {
    read_index = 0;
    return status_ok && register_pointer == 0xFD && quantity == 2 ? 2 : 0;
  }
  int read() {
    if (read_index == 0) {
      ++read_index;
      return (bytes_available >> 8) & 0xFF;
    }
    if (read_index == 1) {
      ++read_index;
      return bytes_available & 0xFF;
    }
    return -1;
  }
};

inline TwoWire Wire;
