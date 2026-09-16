#pragma once

#include <stddef.h>
#include <stdint.h>

inline bool fake_wire_timeout_flag = false;
inline bool fake_wire_reset_required_flag = false;
inline bool fake_accelerometer_present = false;
inline bool fake_accelerometer_short_axis_read = false;
inline int fake_accelerometer_fail_write_register = -1;
inline uint8_t fake_accelerometer_registers[256]{};
inline uint8_t fake_accelerometer_write_regs[64]{};
inline uint8_t fake_accelerometer_write_values[64]{};
inline unsigned fake_accelerometer_write_count = 0;
inline unsigned fake_accelerometer_axis_read_count = 0;

struct TwoWire {
  uint8_t address = 0;
  uint8_t tx[2]{};
  uint8_t tx_length = 0;
  uint8_t register_pointer = 0;
  uint8_t read_index = 0;
  uint8_t read_count = 0;
  unsigned begin_calls = 0;
  unsigned end_calls = 0;
  unsigned set_clock_calls = 0;
  uint32_t last_clock_hz = 0;

  void begin() { ++begin_calls; }
  void end() { ++end_calls; }
  void setClock(uint32_t hz) {
    ++set_clock_calls;
    last_clock_hz = hz;
  }

  void beginTransmission(uint8_t value) {
    address = value;
    tx_length = 0;
  }

  size_t write(uint8_t value) {
    if (tx_length < sizeof(tx)) tx[tx_length++] = value;
    return 1;
  }

  uint8_t endTransmission(bool = true) {
    if (address != 0x18 || !fake_accelerometer_present || tx_length == 0)
      return 4;

    register_pointer = tx[0];
    if (tx_length == 2) {
      const uint8_t reg = static_cast<uint8_t>(tx[0] & 0x7Fu);
      if (fake_accelerometer_fail_write_register == reg) return 4;
      fake_accelerometer_registers[reg] = tx[1];
      if (fake_accelerometer_write_count < 64) {
        fake_accelerometer_write_regs[fake_accelerometer_write_count] = reg;
        fake_accelerometer_write_values[fake_accelerometer_write_count] = tx[1];
        ++fake_accelerometer_write_count;
      }
    }
    return 0;
  }

  uint8_t requestFrom(uint8_t value, uint8_t quantity) {
    read_index = 0;
    read_count = 0;
    if (value != 0x18 || !fake_accelerometer_present) return 0;
    if (quantity == 6) ++fake_accelerometer_axis_read_count;
    if (fake_accelerometer_short_axis_read && quantity == 6) {
      read_count = 5;
      return 5;
    }
    read_count = quantity;
    return quantity;
  }

  int read() {
    if (read_index >= read_count) return -1;
    const uint8_t base = static_cast<uint8_t>(register_pointer & 0x7Fu);
    const uint8_t value = fake_accelerometer_registers[
        static_cast<uint8_t>(base + read_index)];
    ++read_index;
    return value;
  }
};

inline TwoWire Wire;

extern "C" inline bool orunWireTakeTimeoutFlag(void) {
  const bool value = fake_wire_timeout_flag;
  fake_wire_timeout_flag = false;
  return value;
}

extern "C" inline bool orunWireTakeResetRequiredFlag(void) {
  const bool value = fake_wire_reset_required_flag;
  fake_wire_reset_required_flag = false;
  return value;
}
