#pragma once

#include <stdint.h>

namespace orun_tlp {

constexpr uint32_t estimateLoraAirtimeUs(uint8_t payload_bytes,
                                         uint8_t spreading_factor,
                                         uint16_t bandwidth_khz,
                                         uint8_t coding_rate_index,
                                         uint16_t preamble_symbols,
                                         bool crc_enabled,
                                         bool explicit_header) {
  const uint32_t symbol_us =
      (uint32_t(1) << spreading_factor) * 1000UL / bandwidth_khz;
  const uint8_t low_data_rate = symbol_us >= 16000 ? 1 : 0;
  const int32_t numerator = 8 * payload_bytes - 4 * spreading_factor + 28 +
      (crc_enabled ? 16 : 0) - (explicit_header ? 0 : 20);
  const uint32_t denominator = 4 * (spreading_factor - 2 * low_data_rate);
  const uint32_t payload_blocks = numerator > 0
      ? (uint32_t(numerator) + denominator - 1) / denominator : 0;
  const uint32_t payload_symbols =
      8 + payload_blocks * (coding_rate_index + 4);
  return symbol_us * payload_symbols +
      symbol_us * (preamble_symbols * 4 + 17) / 4;
}

}  // namespace orun_tlp
