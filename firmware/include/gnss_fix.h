#pragma once

#include <stdint.h>

namespace orun_tlp {

// Portable GNSS observation consumed by core/application code. Driver callback
// types and parser state stay in the concrete GNSS manager implementation.
struct GnssFix {
  uint32_t utc_epoch_seconds;
  int32_t latitude_e7;
  int32_t longitude_e7;
  int32_t altitude_mm;
  uint16_t hdop_x100;
  uint8_t satellites;
  uint8_t flags;
  // Local callback capture time only; never serialized or persisted.
  uint32_t captured_at_ms = 0;
};

}  // namespace orun_tlp
