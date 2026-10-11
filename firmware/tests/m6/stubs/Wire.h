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

// LIS3DH FIFO model (Stream mode, AN3308): 32 levels; the newest 32 samples
// are kept, FIFO_SRC reports OVRN with FSS=31 when full, Bypass empties it.
inline int16_t fake_fifo_mg[32][3]{};
inline unsigned fake_fifo_head = 0;
inline unsigned fake_fifo_count = 0;
inline unsigned fake_accelerometer_max_request = 0;
inline unsigned fake_accelerometer_fail_reads = 0;  // next N reads fail

inline bool fakeFifoStreaming() {
  return (fake_accelerometer_registers[0x24] & 0x40u) != 0 &&
         (fake_accelerometer_registers[0x2E] & 0xC0u) == 0x80u;
}

inline void fakeFifoClear() {
  fake_fifo_head = 0;
  fake_fifo_count = 0;
}

// One output-data-rate tick of the sensor; ignored unless streaming.
inline void fakeFifoPush(int16_t x_mg, int16_t y_mg, int16_t z_mg) {
  if (!fakeFifoStreaming()) return;
  if (fake_fifo_count == 32) {
    fake_fifo_head = (fake_fifo_head + 1u) % 32u;
    --fake_fifo_count;
  }
  const unsigned tail = (fake_fifo_head + fake_fifo_count) % 32u;
  fake_fifo_mg[tail][0] = x_mg;
  fake_fifo_mg[tail][1] = y_mg;
  fake_fifo_mg[tail][2] = z_mg;
  ++fake_fifo_count;
}

inline uint8_t fakeFifoSource() {
  if (fake_fifo_count == 0) return 0x20u;
  if (fake_fifo_count == 32) return 0x40u | 0x1Fu;
  return static_cast<uint8_t>(fake_fifo_count);
}

struct TwoWire {
  uint8_t address = 0;
  uint8_t tx[2]{};
  uint8_t tx_length = 0;
  uint8_t register_pointer = 0;
  uint8_t read_index = 0;
  uint8_t read_count = 0;
  bool fifo_bytes = false;
  uint8_t fifo_buffer[64]{};
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
      if (reg == 0x2E && (tx[1] & 0xC0u) == 0) fakeFifoClear();
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
    fifo_bytes = false;
    if (value != 0x18 || !fake_accelerometer_present) return 0;
    if (quantity > fake_accelerometer_max_request)
      fake_accelerometer_max_request = quantity;
    if (fake_accelerometer_fail_reads != 0) {
      --fake_accelerometer_fail_reads;
      return 0;
    }
    if (quantity == 6) ++fake_accelerometer_axis_read_count;
    if ((register_pointer & 0x7Fu) == 0x2F) {
      fake_accelerometer_registers[0x2F] = fakeFifoSource();
    } else if ((register_pointer & 0x7Fu) == 0x28 && fakeFifoStreaming() &&
               quantity % 6u == 0 && quantity <= sizeof(fifo_buffer)) {
      // Normal mode, 10-bit left-justified: raw = mg * 16 for mg % 4 == 0.
      fifo_bytes = true;
      for (unsigned i = 0; i < quantity / 6u; ++i) {
        for (unsigned axis = 0; axis < 3; ++axis) {
          int16_t mg = 0;
          if (fake_fifo_count != 0) mg = fake_fifo_mg[fake_fifo_head][axis];
          const uint16_t raw = static_cast<uint16_t>(static_cast<int16_t>(mg * 16));
          fifo_buffer[6u * i + 2u * axis] = static_cast<uint8_t>(raw & 0xFFu);
          fifo_buffer[6u * i + 2u * axis + 1u] = static_cast<uint8_t>(raw >> 8);
        }
        if (fake_fifo_count != 0) {
          fake_fifo_head = (fake_fifo_head + 1u) % 32u;
          --fake_fifo_count;
        }
      }
    }
    if (fake_accelerometer_short_axis_read && quantity == 6) {
      read_count = 5;
      return 5;
    }
    read_count = quantity;
    return quantity;
  }

  int read() {
    if (read_index >= read_count) return -1;
    if (fifo_bytes) return fifo_buffer[read_index++];
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
