#include "network_service.h"

#include <string.h>

#include "monotonic_time.h"
#include "tlp_test_packet.h"

namespace orun_tlp {
namespace {

uint32_t mix(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7FEB352DUL;
  value ^= value >> 15;
  value *= 0x846CA68BUL;
  value ^= value >> 16;
  return value;
}

PacketKey keyFor(const tlp::PositionPacket& packet) {
  PacketKey key{};
  key.source_device_id = packet.source_device_id;
  key.sequence_number = packet.sequence_number;
  return key;
}

}  // namespace

uint32_t deterministicRelayDelay(uint64_t source_device_id,
                                 uint32_t sequence_number,
                                 uint64_t relay_device_id) {
  uint32_t value = static_cast<uint32_t>(source_device_id) ^
      static_cast<uint32_t>(source_device_id >> 32) ^ sequence_number;
  value = mix(value ^ static_cast<uint32_t>(relay_device_id));
  value = mix(value ^ static_cast<uint32_t>(relay_device_id >> 32));
  const uint32_t width = relay_config::kMaximumDelayMs -
      relay_config::kMinimumDelayMs + 1;
  return relay_config::kMinimumDelayMs + value % width;
}

void NetworkService::begin(uint64_t local_device_id, NodeRole role) {
  local_device_id_ = local_device_id;
  role_ = role;
  relay_dedupe_.clear();
  base_dedupe_.clear();
  clearQueue();
  forward_active_ = false;
  relay_diagnostics_ = {};
  base_diagnostics_ = {};
}

void NetworkService::setRole(NodeRole role) {
  if (role == role_) return;
  role_ = role;
  relay_dedupe_.clear();
  base_dedupe_.clear();
  clearQueue();
  forward_active_ = false;
}

void NetworkService::clearQueue() {
  for (auto& entry : queue_) entry.valid = false;
  queued_count_ = 0;
}

NetworkEvent NetworkService::malformedEvent(bool relay_envelope) {
  if (role_ == NodeRole::kRelay) ++relay_diagnostics_.malformed_rejected;
  if (role_ == NodeRole::kBase && relay_envelope)
    ++base_diagnostics_.malformed_relay_envelopes;
  NetworkEvent event{};
  event.kind = NetworkEventKind::kMalformed;
  return event;
}

bool NetworkService::enqueue(const uint8_t* packet, int16_t rssi_dbm,
                             int8_t snr_db, uint32_t due_ms) {
  for (auto& entry : queue_) {
    if (entry.valid) continue;
    entry.valid = true;
    entry.due_ms = due_ms;
    entry.ingress_rssi_dbm = rssi_dbm;
    entry.ingress_snr_db = snr_db;
    memcpy(entry.packet, packet, tlp::kPositionPacketSize);
    ++queued_count_;
    return true;
  }
  return false;  // Drop newest; existing scheduled work keeps its deadline.
}

NetworkEvent NetworkService::receive(const uint8_t* payload, size_t size,
                                     int16_t rssi_dbm, int8_t snr_db,
                                     uint32_t now_ms) {
  if (payload == nullptr || size < 2 || payload[0] != tlp::kProtocolVersion)
    return malformedEvent(size >= 2 && payload != nullptr &&
                          payload[1] == tlp::kPacketTypeRelayForward);

  if (payload[1] == tlp::kPacketTypePosition) {
    tlp::PositionPacket position{};
    if (!tlp::deserializePositionPacket(payload, size, &position))
      return malformedEvent(false);
    NetworkEvent event{};
    event.position = position;
    event.path = NetworkPath::kDirect;
    event.link_rssi_dbm = rssi_dbm;
    event.link_snr_db = snr_db;

    if (role_ == NodeRole::kRelay) {
      ++relay_diagnostics_.valid_packets_received;
      const PacketKey key = keyFor(position);
      if (relay_dedupe_.contains(key)) {
        ++relay_diagnostics_.duplicates_suppressed;
        event.kind = NetworkEventKind::kRelayDuplicate;
        return event;
      }
      event.relay_delay_ms = deterministicRelayDelay(
          position.source_device_id, position.sequence_number, local_device_id_);
      if (!enqueue(payload, rssi_dbm, snr_db,
                   now_ms + event.relay_delay_ms)) {
        ++relay_diagnostics_.queue_drops;
        event.kind = NetworkEventKind::kRelayQueueDrop;
        return event;
      }
      relay_dedupe_.observe(key);
      ++relay_diagnostics_.queued;
      event.kind = NetworkEventKind::kRelayQueued;
      return event;
    }

    if (role_ == NodeRole::kBase) {
      ++base_diagnostics_.direct_observations;
      if (base_dedupe_.observe(keyFor(position))) {
        ++base_diagnostics_.duplicates;
        event.kind = NetworkEventKind::kBaseDuplicate;
      } else {
        ++base_diagnostics_.new_application_packets;
        event.kind = NetworkEventKind::kBaseNew;
      }
      return event;
    }

    event.kind = NetworkEventKind::kIgnoredPosition;
    return event;
  }

  if (payload[1] == tlp::kPacketTypeRelayForward) {
    tlp::RelayForwardPacket envelope{};
    tlp::PositionPacket position{};
    const auto status = tlp::deserializeRelayForwardPacket(
        payload, size, &envelope, &position);
    if (role_ == NodeRole::kRelay) {
      if (status == tlp::RelayDecodeStatus::kOk ||
          status == tlp::RelayDecodeStatus::kNestedRelay) {
        ++relay_diagnostics_.nested_rejected;
        NetworkEvent event{};
        event.kind = NetworkEventKind::kRelayNestedRejected;
        return event;
      }
      return malformedEvent(true);
    }
    if (status != tlp::RelayDecodeStatus::kOk)
      return malformedEvent(true);
    if (role_ != NodeRole::kBase) {
      NetworkEvent event{};
      event.kind = NetworkEventKind::kIgnoredPosition;
      return event;
    }

    NetworkEvent event{};
    event.path = NetworkPath::kRelay;
    event.position = position;
    event.relay_device_id = envelope.relay_device_id;
    event.ingress_rssi_dbm = envelope.ingress_rssi_dbm;
    event.ingress_snr_db = envelope.ingress_snr_db;
    event.link_rssi_dbm = rssi_dbm;
    event.link_snr_db = snr_db;
    ++base_diagnostics_.relay_observations;
    if (base_dedupe_.observe(keyFor(position))) {
      ++base_diagnostics_.duplicates;
      event.kind = NetworkEventKind::kBaseDuplicate;
    } else {
      ++base_diagnostics_.new_application_packets;
      event.kind = NetworkEventKind::kBaseNew;
    }
    return event;
  }

  return malformedEvent(false);
}

bool NetworkService::takeDueForward(uint32_t now_ms,
                                    tlp::RelayForwardPacket* packet) {
  if (packet == nullptr || role_ != NodeRole::kRelay || forward_active_)
    return false;
  for (auto& entry : queue_) {
    if (!entry.valid || !monotonic::reached(now_ms, entry.due_ms)) continue;
    packet->relay_device_id = local_device_id_;
    packet->hop_count = tlp::kRelayHopCount;
    packet->original_length = tlp::kPositionPacketSize;
    packet->ingress_rssi_dbm = entry.ingress_rssi_dbm;
    packet->ingress_snr_db = entry.ingress_snr_db;
    memcpy(packet->original_packet, entry.packet, tlp::kPositionPacketSize);
    entry.valid = false;
    --queued_count_;
    return true;
  }
  return false;
}

void NetworkService::onForwardTxStarted() {
  forward_active_ = true;
  ++relay_diagnostics_.forwards_attempted;
}

void NetworkService::onForwardTxResult(bool success) {
  forward_active_ = false;
  if (success) ++relay_diagnostics_.forwards_completed;
  else ++relay_diagnostics_.forward_failures;
}

}  // namespace orun_tlp
