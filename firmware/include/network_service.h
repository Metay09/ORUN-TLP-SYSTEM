#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node_role.h"
#include "packet_dedupe.h"
#include "relay_config.h"
#include "tlp_relay_forward_packet.h"

namespace orun_tlp {

enum class NetworkEventKind : uint8_t {
  kNone,
  kIgnoredPosition,
  kMalformed,
  kRelayQueued,
  kRelayDuplicate,
  kRelayQueueDrop,
  kRelayNestedRejected,
  kBaseNew,
  kBaseDuplicate,
};

enum class NetworkPath : uint8_t { kDirect, kRelay };

struct NetworkEvent {
  NetworkEventKind kind = NetworkEventKind::kNone;
  NetworkPath path = NetworkPath::kDirect;
  tlp::PositionPacket position{};
  uint64_t relay_device_id = 0;
  int16_t ingress_rssi_dbm = 0;
  int8_t ingress_snr_db = 0;
  int16_t link_rssi_dbm = 0;
  int8_t link_snr_db = 0;
  uint32_t relay_delay_ms = 0;
};

struct RelayDiagnostics {
  uint32_t valid_packets_received = 0;
  uint32_t duplicates_suppressed = 0;
  uint32_t queued = 0;
  uint32_t queue_drops = 0;
  uint32_t forwards_attempted = 0;
  uint32_t forwards_completed = 0;
  uint32_t forward_failures = 0;
  uint32_t malformed_rejected = 0;
  uint32_t nested_rejected = 0;
};

struct BaseDiagnostics {
  uint32_t new_application_packets = 0;
  uint32_t duplicates = 0;
  uint32_t direct_observations = 0;
  uint32_t relay_observations = 0;
  uint32_t malformed_relay_envelopes = 0;
};

uint32_t deterministicRelayDelay(uint64_t source_device_id,
                                 uint32_t sequence_number,
                                 uint64_t relay_device_id);

class NetworkService {
 public:
  // Intentionally unsynchronized: RadioManager's application/loop owner is
  // the only caller. Radio callbacks hand off immutable events first.
  void begin(uint64_t local_device_id, NodeRole role);
  void setRole(NodeRole role);
  NodeRole role() const { return role_; }

  // B4 seam: forwarding is an independent network behavior, not a permanent
  // device type. Legacy role transitions still install their historical
  // default until the resolved runtime config is wired through RadioManager.
  void setRelayForwardingEnabled(bool enabled);
  bool relayForwardingEnabled() const { return relay_forwarding_enabled_; }

  NetworkEvent receive(const uint8_t* payload, size_t size, int16_t rssi_dbm,
                       int8_t snr_db, uint32_t now_ms);
  bool takeDueForward(uint32_t now_ms, tlp::RelayForwardPacket* packet);
  void onForwardTxStarted();
  void onForwardTxResult(bool success);
  uint8_t queuedCount() const { return queued_count_; }
  const RelayDiagnostics& relayDiagnostics() const { return relay_diagnostics_; }
  const BaseDiagnostics& baseDiagnostics() const { return base_diagnostics_; }

 private:
  struct QueueEntry {
    bool valid = false;
    uint32_t due_ms = 0;
    int16_t ingress_rssi_dbm = 0;
    int8_t ingress_snr_db = 0;
    uint8_t packet[tlp::kPositionPacketSize]{};
  };

  NetworkEvent malformedEvent(bool relay_envelope);
  bool enqueue(const uint8_t* packet, int16_t rssi_dbm, int8_t snr_db,
               uint32_t due_ms);
  void clearQueue();
  void resetRelayState();

  uint64_t local_device_id_ = 0;
  NodeRole role_ = NodeRole::kBase;
  bool relay_forwarding_enabled_ = false;
  PacketDedupeCache<relay_config::kRelayDedupeSize> relay_dedupe_{};
  PacketDedupeCache<relay_config::kBaseDedupeSize> base_dedupe_{};
  QueueEntry queue_[relay_config::kForwardQueueSize]{};
  uint8_t queued_count_ = 0;
  bool forward_active_ = false;
  RelayDiagnostics relay_diagnostics_{};
  BaseDiagnostics base_diagnostics_{};
};

}  // namespace orun_tlp
