#include "observation_store_control.h"

#include <string.h>

namespace orun_tlp {
namespace observation_store_control {
namespace {

bool validRecordKind(osf::RecordKind kind) {
  return kind == osf::RecordKind::kPeriodic ||
         kind == osf::RecordKind::kEvent ||
         kind == osf::RecordKind::kResult;
}

bool zeroPadding(const uint8_t* bytes, size_t begin, size_t end) {
  for (size_t i = begin; i < end; ++i)
    if (bytes[i] != 0U) return false;
  return true;
}

}  // namespace

bool encodeExactObject(const ExactObject& value,
                       uint8_t out[kExactObjectPayloadSize]) {
  if (out == nullptr || !value.identity.valid() ||
      !validRecordKind(value.record_kind) || value.object_size == 0U ||
      value.object_size > kMaxProtectedObjectSize)
    return false;

  memset(out, 0, kExactObjectPayloadSize);
  osf::put64(out + 0U, value.identity.incarnation);
  osf::put32(out + 8U, value.identity.sequence);
  out[12] = static_cast<uint8_t>(value.record_kind);
  out[13] = value.object_size;
  memcpy(out + 16U, value.object, value.object_size);
  return true;
}

bool decodeExactObject(const uint8_t* bytes, size_t size, ExactObject& value) {
  value = ExactObject();
  if (bytes == nullptr || size != kExactObjectPayloadSize) return false;
  value.identity.incarnation = osf::get64(bytes + 0U);
  value.identity.sequence = osf::get32(bytes + 8U);
  value.record_kind = static_cast<osf::RecordKind>(bytes[12]);
  value.object_size = bytes[13];
  if (!value.identity.valid() || !validRecordKind(value.record_kind) ||
      value.object_size == 0U || value.object_size > kMaxProtectedObjectSize ||
      bytes[14] != 0U || bytes[15] != 0U ||
      !zeroPadding(bytes, 16U + value.object_size, kExactObjectPayloadSize))
    return false;
  memcpy(value.object, bytes + 16U, value.object_size);
  return true;
}

bool encodeOpenOccurrence(const OpenOccurrence& value,
                          uint8_t out[kOpenOccurrencePayloadSize]) {
  if (out == nullptr || value.event_type == 0U || value.occurrence_id == 0U)
    return false;
  memset(out, 0, kOpenOccurrencePayloadSize);
  out[0] = value.event_type;
  out[1] = value.reason_code;
  out[2] = value.context_kind;
  osf::put32(out + 4U, value.context_value);
  osf::put64(out + 8U, value.occurrence_id);
  return true;
}

bool decodeOpenOccurrence(const uint8_t* bytes, size_t size,
                          OpenOccurrence& value) {
  value = OpenOccurrence();
  if (bytes == nullptr || size != kOpenOccurrencePayloadSize) return false;
  value.event_type = bytes[0];
  value.reason_code = bytes[1];
  value.context_kind = bytes[2];
  value.context_value = osf::get32(bytes + 4U);
  value.occurrence_id = osf::get64(bytes + 8U);
  return value.event_type != 0U && bytes[3] == 0U &&
         value.occurrence_id != 0U;
}

bool encodeResultGuard(const ResultGuard& value,
                       uint8_t out[kResultGuardPayloadSize]) {
  if (out == nullptr || value.gateway_device_id == 0U ||
      value.gateway_policy_floor == UINT32_MAX ||
      value.gateway_grant_generation == 0U ||
      value.gateway_grant_generation == UINT32_MAX ||
      value.command_id == 0U || value.opcode == 0U ||
      !value.result_identity.valid())
    return false;
  memset(out, 0, kResultGuardPayloadSize);
  osf::put64(out + 0U, value.gateway_device_id);
  osf::put32(out + 8U, value.gateway_policy_floor);
  osf::put32(out + 12U, value.gateway_grant_generation);
  osf::put64(out + 16U, value.command_id);
  out[24] = value.opcode;
  memcpy(out + 28U, value.expected_state_token,
         sizeof(value.expected_state_token));
  osf::put32(out + 40U, value.tracking_interval_seconds);
  osf::put32(out + 44U, value.battery_capacity_mah);
  osf::put64(out + 48U, value.result_identity.incarnation);
  osf::put32(out + 56U, value.result_identity.sequence);
  return true;
}

bool decodeResultGuard(const uint8_t* bytes, size_t size, ResultGuard& value) {
  value = ResultGuard();
  if (bytes == nullptr || size != kResultGuardPayloadSize) return false;
  value.gateway_device_id = osf::get64(bytes + 0U);
  value.gateway_policy_floor = osf::get32(bytes + 8U);
  value.gateway_grant_generation = osf::get32(bytes + 12U);
  value.command_id = osf::get64(bytes + 16U);
  value.opcode = bytes[24];
  memcpy(value.expected_state_token, bytes + 28U,
         sizeof(value.expected_state_token));
  value.tracking_interval_seconds = osf::get32(bytes + 40U);
  value.battery_capacity_mah = osf::get32(bytes + 44U);
  value.result_identity.incarnation = osf::get64(bytes + 48U);
  value.result_identity.sequence = osf::get32(bytes + 56U);
  return value.gateway_device_id != 0U &&
         value.gateway_policy_floor != UINT32_MAX &&
         value.gateway_grant_generation != 0U &&
         value.gateway_grant_generation != UINT32_MAX &&
         value.command_id != 0U && value.opcode != 0U &&
         bytes[25] == 0U && bytes[26] == 0U && bytes[27] == 0U &&
         value.result_identity.valid();
}

bool encodeStoreState(const StoreState& value,
                      uint8_t out[kStoreStatePayloadSize]) {
  if (out == nullptr) return false;
  if (value.capacity_lost_total != value.capacity_lost_periodic +
                                       value.capacity_lost_event +
                                       value.capacity_lost_result)
    return false;
  if (value.rotation_pending) {
    if (value.target_page == UINT16_MAX ||
        value.target_old_generation == 0U ||
        value.target_new_generation == 0U ||
        value.target_new_generation <= value.target_old_generation)
      return false;
  } else if (value.target_page != UINT16_MAX ||
             value.target_old_generation != 0U ||
             value.target_new_generation != 0U) {
    return false;
  }
  if ((value.first_lost_sequence == 0U) !=
      (value.last_lost_sequence == 0U))
    return false;
  if (value.first_lost_sequence != 0U &&
      value.first_lost_sequence > value.last_lost_sequence)
    return false;

  memset(out, 0, kStoreStatePayloadSize);
  osf::put32(out + 0U, value.capacity_lost_total);
  osf::put32(out + 4U, value.capacity_lost_periodic);
  osf::put32(out + 8U, value.capacity_lost_event);
  osf::put32(out + 12U, value.capacity_lost_result);
  out[16] = value.rotation_pending ? 1U : 0U;
  osf::put16(out + 20U, value.target_page);
  osf::put64(out + 24U, value.target_old_generation);
  osf::put64(out + 32U, value.target_new_generation);
  osf::put32(out + 40U, value.first_lost_sequence);
  osf::put32(out + 44U, value.last_lost_sequence);
  return true;
}

bool decodeStoreState(const uint8_t* bytes, size_t size, StoreState& value) {
  value = StoreState();
  if (bytes == nullptr || size != kStoreStatePayloadSize) return false;
  value.capacity_lost_total = osf::get32(bytes + 0U);
  value.capacity_lost_periodic = osf::get32(bytes + 4U);
  value.capacity_lost_event = osf::get32(bytes + 8U);
  value.capacity_lost_result = osf::get32(bytes + 12U);
  if (bytes[16] > 1U || bytes[17] != 0U || bytes[18] != 0U ||
      bytes[19] != 0U || bytes[22] != 0U || bytes[23] != 0U)
    return false;
  value.rotation_pending = bytes[16] == 1U;
  value.target_page = osf::get16(bytes + 20U);
  value.target_old_generation = osf::get64(bytes + 24U);
  value.target_new_generation = osf::get64(bytes + 32U);
  value.first_lost_sequence = osf::get32(bytes + 40U);
  value.last_lost_sequence = osf::get32(bytes + 44U);

  if (value.capacity_lost_total != value.capacity_lost_periodic +
                                       value.capacity_lost_event +
                                       value.capacity_lost_result)
    return false;
  if (value.rotation_pending) {
    if (value.target_page == UINT16_MAX ||
        value.target_old_generation == 0U ||
        value.target_new_generation == 0U ||
        value.target_new_generation <= value.target_old_generation)
      return false;
  } else if (value.target_page != UINT16_MAX ||
             value.target_old_generation != 0U ||
             value.target_new_generation != 0U) {
    return false;
  }
  if ((value.first_lost_sequence == 0U) !=
      (value.last_lost_sequence == 0U))
    return false;
  return value.first_lost_sequence == 0U ||
         value.first_lost_sequence <= value.last_lost_sequence;
}

bool sameExactObjectKey(const ExactObject& a, const ExactObject& b) {
  return a.identity.incarnation == b.identity.incarnation &&
         a.identity.sequence == b.identity.sequence;
}

bool sameOpenOccurrenceKey(const OpenOccurrence& a, const OpenOccurrence& b) {
  return a.event_type == b.event_type && a.reason_code == b.reason_code &&
         a.context_kind == b.context_kind &&
         a.context_value == b.context_value;
}

bool sameResultGuardKey(const ResultGuard& a, const ResultGuard& b) {
  return a.gateway_device_id == b.gateway_device_id &&
         a.gateway_policy_floor == b.gateway_policy_floor &&
         a.gateway_grant_generation == b.gateway_grant_generation &&
         a.command_id == b.command_id;
}

}  // namespace observation_store_control
}  // namespace orun_tlp
