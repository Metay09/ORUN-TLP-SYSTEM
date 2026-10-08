#pragma once

#include <stddef.h>
#include <stdint.h>

#include "observation_store_format.h"

namespace orun_tlp {
namespace observation_store_control {

namespace osf = observation_store_format;

constexpr size_t kExactObjectPayloadSize = 116U;
constexpr size_t kMaxProtectedObjectSize = 100U;
constexpr size_t kOpenOccurrencePayloadSize = 16U;
constexpr size_t kResultGuardPayloadSize = 60U;
constexpr size_t kStoreStatePayloadSize = 48U;

static_assert(kExactObjectPayloadSize <= osf::kControlPayloadSize,
              "exact object control must fit one control slot");
static_assert(kMaxProtectedObjectSize == 100U,
              "SF5B maximum PRODUCT_SECURE object changed");

struct ExactObject {
  osf::RecordIdentity identity{};
  osf::RecordKind record_kind = osf::RecordKind::kPeriodic;
  uint8_t object_size = 0;
  uint8_t object[kMaxProtectedObjectSize]{};
};

struct OpenOccurrence {
  uint8_t event_type = 0;
  uint8_t reason_code = 0;
  uint8_t context_kind = 0;
  uint32_t context_value = 0;
  uint64_t occurrence_id = 0;
};

struct ResultGuard {
  uint64_t gateway_device_id = 0;
  uint32_t gateway_policy_floor = 0;
  uint32_t gateway_grant_generation = 0;
  uint64_t command_id = 0;
  uint8_t opcode = 0;
  uint8_t expected_state_token[12]{};
  uint32_t tracking_interval_seconds = 0;
  uint32_t battery_capacity_mah = 0;
  osf::RecordIdentity result_identity{};
};

struct StoreState {
  uint32_t capacity_lost_total = 0;
  uint32_t capacity_lost_periodic = 0;
  uint32_t capacity_lost_event = 0;
  uint32_t capacity_lost_result = 0;
  bool rotation_pending = false;
  uint16_t target_page = UINT16_MAX;
  uint64_t target_old_generation = 0;
  uint64_t target_new_generation = 0;
  uint32_t first_lost_sequence = 0;
  uint32_t last_lost_sequence = 0;
};

bool encodeExactObject(const ExactObject& value,
                       uint8_t out[kExactObjectPayloadSize]);
bool decodeExactObject(const uint8_t* bytes, size_t size, ExactObject& value);

bool encodeOpenOccurrence(const OpenOccurrence& value,
                          uint8_t out[kOpenOccurrencePayloadSize]);
bool decodeOpenOccurrence(const uint8_t* bytes, size_t size,
                          OpenOccurrence& value);

bool encodeResultGuard(const ResultGuard& value,
                       uint8_t out[kResultGuardPayloadSize]);
bool decodeResultGuard(const uint8_t* bytes, size_t size, ResultGuard& value);

bool encodeStoreState(const StoreState& value,
                      uint8_t out[kStoreStatePayloadSize]);
bool decodeStoreState(const uint8_t* bytes, size_t size, StoreState& value);

bool sameExactObjectKey(const ExactObject& a, const ExactObject& b);
bool sameOpenOccurrenceKey(const OpenOccurrence& a, const OpenOccurrence& b);
bool sameResultGuardKey(const ResultGuard& a, const ResultGuard& b);

}  // namespace observation_store_control
}  // namespace orun_tlp
