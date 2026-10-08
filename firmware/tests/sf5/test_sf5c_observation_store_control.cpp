#include <assert.h>
#include <string.h>

#include "observation_store_control.h"

namespace osc = orun_tlp::observation_store_control;
namespace osf = orun_tlp::observation_store_format;

static void testExactObject() {
  osc::ExactObject value;
  value.identity.incarnation = 5U;
  value.identity.sequence = 7U;
  value.record_kind = osf::RecordKind::kPeriodic;
  value.object_size = 100U;
  for (unsigned i = 0; i < value.object_size; ++i)
    value.object[i] = static_cast<uint8_t>(i);

  uint8_t bytes[osc::kExactObjectPayloadSize];
  assert(osc::encodeExactObject(value, bytes));
  osc::ExactObject decoded;
  assert(osc::decodeExactObject(bytes, sizeof(bytes), decoded));
  assert(osc::sameExactObjectKey(value, decoded));
  assert(decoded.object_size == value.object_size);
  assert(memcmp(value.object, decoded.object, value.object_size) == 0);

  // A 100-byte object exactly fills bytes 16..115, so there is no tail
  // padding to corrupt in the maximum-size case. Reserved bytes must still be
  // rejected.
  bytes[14] = 1U;
  assert(!osc::decodeExactObject(bytes, sizeof(bytes), decoded));

  // Use a 99-byte object to exercise the canonical zero-padding check at the
  // final byte of the 116-byte control payload.
  value.object_size = 99U;
  assert(osc::encodeExactObject(value, bytes));
  bytes[115] = 1U;
  assert(!osc::decodeExactObject(bytes, sizeof(bytes), decoded));
}

static void testOpenOccurrence() {
  osc::OpenOccurrence value;
  value.event_type = 1U;
  value.reason_code = 2U;
  value.context_kind = 1U;
  value.context_value = 3700U;
  value.occurrence_id = 99U;

  uint8_t bytes[osc::kOpenOccurrencePayloadSize];
  assert(osc::encodeOpenOccurrence(value, bytes));
  osc::OpenOccurrence decoded;
  assert(osc::decodeOpenOccurrence(bytes, sizeof(bytes), decoded));
  assert(osc::sameOpenOccurrenceKey(value, decoded));
  assert(decoded.occurrence_id == 99U);
}

static void testOpenOccurrenceKeySemantics() {
  osc::OpenOccurrence battery_a;
  battery_a.event_type = 3U;     // BATTERY_STATE
  battery_a.reason_code = 1U;    // LOW
  battery_a.context_kind = 1U;   // BATTERY_MV
  battery_a.context_value = 3600U;
  battery_a.occurrence_id = 10U;

  osc::OpenOccurrence battery_b = battery_a;
  battery_b.context_value = 3500U;
  battery_b.occurrence_id = 11U;
  assert(osc::sameOpenOccurrenceKey(battery_a, battery_b));

  osc::OpenOccurrence subsystem_a;
  subsystem_a.event_type = 4U;   // SUBSYSTEM_FAULT
  subsystem_a.context_kind = 2U; // SUBSYSTEM_ID
  subsystem_a.context_value = 1U;
  subsystem_a.occurrence_id = 20U;

  osc::OpenOccurrence subsystem_b = subsystem_a;
  subsystem_b.context_value = 2U;
  subsystem_b.occurrence_id = 21U;
  assert(!osc::sameOpenOccurrenceKey(subsystem_a, subsystem_b));
}

static void testResultGuard() {
  osc::ResultGuard value;
  value.gateway_device_id = 2U;
  value.gateway_policy_floor = 3U;
  value.gateway_grant_generation = 4U;
  value.command_id = 5U;
  value.opcode = 1U;
  value.tracking_interval_seconds = 900U;
  value.battery_capacity_mah = 4000U;
  value.result_identity.incarnation = 7U;
  value.result_identity.sequence = 8U;
  for (unsigned i = 0; i < sizeof(value.expected_state_token); ++i)
    value.expected_state_token[i] = static_cast<uint8_t>(10U + i);

  uint8_t bytes[osc::kResultGuardPayloadSize];
  assert(osc::encodeResultGuard(value, bytes));
  osc::ResultGuard decoded;
  assert(osc::decodeResultGuard(bytes, sizeof(bytes), decoded));
  assert(osc::sameResultGuardKey(value, decoded));
  assert(memcmp(value.expected_state_token, decoded.expected_state_token,
                sizeof(value.expected_state_token)) == 0);
  assert(decoded.result_identity.sequence == 8U);
}

static void testStoreState() {
  osc::StoreState state;
  state.capacity_lost_periodic = 2U;
  state.capacity_lost_event = 1U;
  state.capacity_lost_total = 3U;
  state.rotation_pending = true;
  state.target_page = 4U;
  state.target_old_generation = 6U;
  state.target_new_generation = 9U;
  state.first_retired_sequence = 10U;
  state.last_retired_sequence = 12U;

  uint8_t bytes[osc::kStoreStatePayloadSize];
  assert(osc::encodeStoreState(state, bytes));
  osc::StoreState decoded;
  assert(osc::decodeStoreState(bytes, sizeof(bytes), decoded));
  assert(decoded.rotation_pending);
  assert(decoded.capacity_lost_total == 3U);
  assert(decoded.target_page == 4U);

  state.rotation_pending = false;
  state.target_page = UINT16_MAX;
  state.target_old_generation = 0U;
  state.target_new_generation = 0U;
  assert(osc::encodeStoreState(state, bytes));
  assert(osc::decodeStoreState(bytes, sizeof(bytes), decoded));
  assert(!decoded.rotation_pending);

  state.capacity_lost_total = 2U;
  assert(!osc::encodeStoreState(state, bytes));

  state.capacity_lost_periodic = UINT32_MAX;
  state.capacity_lost_event = 1U;
  state.capacity_lost_result = 0U;
  state.capacity_lost_total = 0U;  // wrapped 32-bit sum must not validate.
  assert(!osc::encodeStoreState(state, bytes));
}

int main() {
  testExactObject();
  testOpenOccurrence();
  testOpenOccurrenceKeySemantics();
  testResultGuard();
  testStoreState();
  return 0;
}
