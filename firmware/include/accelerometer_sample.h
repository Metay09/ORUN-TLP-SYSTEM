#pragma once

#include <stdint.h>

namespace orun_tlp {

// Portable physical observation for activity processing. Values are signed
// milli-g (mg), captured from the configured sensor sample. This is not a
// generic telemetry packet and is never transmitted directly by M6A.
struct AccelerometerSample {
  constexpr AccelerometerSample(uint32_t captured_at_ms_value = 0,
                                int16_t x_mg_value = 0,
                                int16_t y_mg_value = 0,
                                int16_t z_mg_value = 0)
      : captured_at_ms(captured_at_ms_value),
        x_mg(x_mg_value),
        y_mg(y_mg_value),
        z_mg(z_mg_value) {}

  uint32_t captured_at_ms;
  int16_t x_mg;
  int16_t y_mg;
  int16_t z_mg;
};

}  // namespace orun_tlp
