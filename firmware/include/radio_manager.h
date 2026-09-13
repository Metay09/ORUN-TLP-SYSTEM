#pragma once

#include <stdint.h>
#include "network_service.h"
#include "sequence_source.h"

namespace orun_tlp {

struct GnssFix;

struct RadioEventDiagnostics {
  uint32_t rx_queue_drops = 0;
  uint32_t oversized_rx_drops = 0;
  uint32_t control_event_drops = 0;
  uint32_t stale_rx_events = 0;
  uint32_t stale_tx_results = 0;
};

class RadioManager {
 public:
  // Application API is loop-owner-only, including diagnostics/accessors.
  // Dependency callbacks publish handoff state while holding the driver gate.
  // Boot initialization captures deviceId and binds local encoding's sequence
  // source even on false; the return value reports radio readiness only.
  bool begin(SequenceSource& sequences);
  void update(bool allow_test_beacon = true);
  void setRole(NodeRole role); // Request; installed by update after quiescence.
  NodeRole role() const { return network_.role(); }
  bool canSend() const;
  bool isTransmitting() const { return tx_in_progress_; }
  bool encodePosition(const GnssFix& fix, uint8_t* payload, uint64_t& identity);
  // A live capture timestamp adds a final freshness gate; backlog omits it.
  bool sendPositionPacket(const uint8_t* payload, const uint32_t* captured_at_ms = nullptr);
  uint64_t deviceId() const;
  uint32_t txAttempts() const { return tx_attempts_; }
  uint32_t txTimeouts() const { return tx_timeouts_; }
  uint32_t localTxFailures() const { return local_tx_failures_; }
  RadioEventDiagnostics eventDiagnostics() const;
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

  enum class TxResult : uint8_t { kNone, kDone, kTimeout };
  enum class RxRestoreState : uint8_t {
    kNone,
    kRequired,
  };

  void enqueueRxEvent(const uint8_t* payload, uint16_t size, int16_t rssi,
                      int8_t snr);
  void postTxResult(TxResult result);
  void postRxTimeout();
  void postRxError();
  void processCallbackEvents();
  void processReceivedEvents();
  bool serviceRxRestore();
  void requestRxRestore();
  void startTxOperation(); // Driver gate held; clears old hardware IRQ work.
  void applyPendingRole(); // Driver gate held; previous TX terminal.
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
  bool tx_in_progress_ = false;
  TxKind tx_kind_ = TxKind::kNone;
  RxRestoreState rx_restore_state_ = RxRestoreState::kNone;
  uint32_t role_epoch_ = 0;
  uint32_t tx_role_epoch_ = 0;
  uint32_t tx_generation_ = 0;
  uint32_t armed_tx_generation_ = 0;
  uint32_t pending_tx_generation_ = 0;
  uint32_t tx_started_ms_ = 0;
  NodeRole requested_role_ = NodeRole::kBase;
  bool role_transition_pending_ = false;
  bool accept_rx_events_ = true;
  TxResult pending_tx_result_ = TxResult::kNone;
  uint32_t pending_rx_timeouts_ = 0;
  uint32_t pending_rx_errors_ = 0;
  NetworkService network_{};
  uint32_t tx_attempts_ = 0, local_tx_failures_ = 0;
  uint32_t tx_timeouts_ = 0;
  RadioEventDiagnostics event_diagnostics_{};
};

}  // namespace orun_tlp
