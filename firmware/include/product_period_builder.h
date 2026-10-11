#pragma once

#include <stdint.h>

#include "activity_period_evidence.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {

// Facts of one finished report period, as the runtime owners know them.
// GNSS is the only location producer today (location_owner.h).
struct PeriodicLocationInput {
  bool valid = false;  // a fresh accepted fix belongs to this period
  int32_t latitude_e7 = 0;
  int32_t longitude_e7 = 0;
  int32_t altitude_mm = 0;
  bool altitude_valid = false;
  uint16_t hdop_x100 = 0;
  uint8_t satellites = 0;
  bool age_valid = false;  // from trusted monotonic time only
  uint32_t age_seconds = 0;
};

struct PeriodicHealthInput {
  bool valid = false;
  uint16_t flags = 0;  // tlp::kHealth* bits
  uint32_t gnss_failures = 0;
  uint32_t sensor_failures = 0;
  uint32_t storage_failures = 0;
  uint32_t radio_failures = 0;
};

struct PeriodicRecordInputs {
  uint64_t record_incarnation = 0;
  uint32_t record_sequence = 0;
  uint32_t period_duration_seconds = 0;
  bool period_end_time_valid = false;  // trusted UTC at the period end
  uint32_t period_end_epoch_seconds = 0;
  PeriodicLocationInput location;
  // Linked to exactly this period (linkActivityPeriodEvidence) or empty.
  ActivityPeriodEvidence activity;
  bool battery_valid = false;
  uint16_t battery_mv = 0;
  uint8_t battery_state = tlp::kBatteryStateUnknown;  // BatteryState value
  PeriodicHealthInput health;
};

// Maps one finished report period to the canonical SF5B PERIODIC schema-v1
// plaintext (ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md section 7): validity bits
// decide and values behind a clear bit are zeroed; location age saturates at
// 0xFFFE; activity counts only when its evidence is classified and covers this
// exact period, with quality following coverage; failure counters saturate at
// 255. Returns the codec verdict; `output` is untouched on failure.
//
// Record identity comes from the caller. The durable SF5C allocator is not
// attached yet, so today's only caller is the USB dry run in main.cpp.
tlp::ProductSecureCodecResult buildPeriodicPlaintext(
    const PeriodicRecordInputs& inputs,
    tlp::PeriodicObservationPlaintext* output);

}  // namespace orun_tlp
