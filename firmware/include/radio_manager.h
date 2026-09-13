#pragma once

#include <stdint.h>
#include "network_service.h"
#include "sequence_source.h"

namespace orun_tlp {

struct GnssFix;

class RadioManager {
 public:
  bool begin(SequenceSource& sequences);
  void update(bool allow_test_beacon = true);
  void setRole(NodeRole role);
  NodeRole role() const { return network_.role(); }
  bool canSend() const;
  bool isTransmitting() const { return tx_in_progress_; }
  bool encodePosition(const GnssFix& fix, uint8_t* payload, uint64_t& identity);
  bool sendPositionPacket(const uint8_t* payload);
  uint64_t deviceId() const;
  uint32_t txAttempts() const { return tx_attempts_; }
  uint32_t txTimeouts() const { return tx_timeouts_; }
  uint32_t localTxFailures() const { return local_tx_failures_; }
  const RelayDiagnostics& relayDiagnostics() const {
    return network_.relayDiagnostics();
  }
  const BaseDiagnostics& baseDiagnostics() const {
    return network_.baseDiagnostics();
  }

 private:
  static void onTxDone();
  static void onTxTimeout();
  static void onRxDone(uint8_t* payload, uint16_t size, int16_t rssi,
                       int8_t snr);
  static void onRxTimeout();
  static void onRxError();

  void sendTestPacket();
  void scheduleNextTransmission(uint32_t now);
  void handleReceivedPacket(const uint8_t* payload, uint16_t size,
                            int16_t rssi, int8_t snr);
  void sendDueRelay(uint32_t now);

  enum class TxKind : uint8_t { kNone, kTest, kPosition, kRelay };

  uint64_t device_id_ = 0;
  SequenceSource* sequences_ = nullptr;
  uint32_t sequence_number_ = 0;
  uint32_t next_tx_at_ms_ = 0;
  bool ready_ = false;
  volatile bool tx_in_progress_ = false;
  TxKind tx_kind_ = TxKind::kNone;
  NetworkService network_{};
  uint32_t tx_attempts_ = 0, local_tx_failures_ = 0;
  volatile uint32_t tx_timeouts_ = 0;
};

}  // namespace orun_tlp
