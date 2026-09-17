#pragma once

#include <stdint.h>
#include "device_identity.h"
#include "network_service.h"
#include "radio_listen_policy.h"
#include "sequence_source.h"

namespace orun_tlp {

struct GnssFix;

enum class RadioListenState : uint8_t {
  kRxContinuous,
  kRxWindowOpen,
  kAsleep,
};

const char* radioListenStateName(RadioListenState state);

struct RadioListenDiagnostics {
  RadioListenPolicy listen_policy = RadioListenPolicy::kContinuous;
  RadioListenState listen_state = RadioListenState::kRxContinuous;
  uint32_t windows_opened = 0;
  uint32_t sleep_entries = 0;
  uint32_t wakes_for_tx = 0;
  uint32_t rx_events_in_window = 0;
  uint32_t stale_restores_while_asleep = 0;
  uint32_t estimated_rx_ms = 0;
};

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
  // Production composition injects identity before begin(); begin retains a
  // RAK-provider fallback for legacy host seams. SequenceSource remains bound
  // here because the disabled M1 TEST-beacon compatibility path still uses it.
  bool begin(SequenceSource& sequences);
  void setDeviceIdentity(DeviceIdentity identity) {
    device_id_ = identity.legacyUint64();
    identity_configured_ = true;
  }
  DeviceIdentity deviceIdentity() const {
    return DeviceIdentity::fromLegacyUint64(device_id_);
  }
  void update(bool allow_test_beacon = true);
  void setRole(NodeRole role); // Request; installed by update after quiescence.
  NodeRole role() const { return network_.role(); }
  // B4 independent forwarding control. Applies synchronously only when the
  // radio owner can quiesce safely; false means the loop owner should retry.
  bool setRelayForwardingEnabled(bool enabled);
  bool relayForwardingEnabled() const {
    return network_.relayForwardingEnabled();
  }
  bool canSend() const;
  bool isTransmitting() const { return tx_in_progress_; }
  // Compatibility shim for existing host seams. Production PositionFlow maps
  // GNSS values through legacy_position_mapping before transport.
  bool encodePosition(const GnssFix& fix, uint8_t* payload, uint64_t& identity);
  // A live capture timestamp adds a final freshness gate; backlog omits it.
  bool sendPositionPacket(const uint8_t* payload, const uint32_t* captured_at_ms = nullptr);
  uint64_t deviceId() const;
  uint32_t txAttempts() const { return tx_attempts_; }
  uint32_t txTimeouts() const { return tx_timeouts_; }
  uint32_t localTxFailures() const { return local_tx_failures_; }
  RadioEventDiagnostics eventDiagnostics() const;
  RadioListenDiagnostics listenDiagnostics() const;
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
  bool serviceRxRestore(uint32_t now);
  bool serviceWindowDeadline(uint32_t now);
  void requestRxRestore();
  RadioListenPolicy desiredListenPolicy() const;
  void reconcileListenPolicy(uint32_t now);
  void openListenWindow(uint32_t now);
  void markRxStarted(uint32_t now);
  void markRxStopped(uint32_t now);
  void foldRxAccounting(uint32_t now);
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
  bool identity_configured_ = false;
  bool ready_ = false;
  bool tx_in_progress_ = false;
  TxKind tx_kind_ = TxKind::kNone;
  RxRestoreState rx_restore_state_ = RxRestoreState::kNone;
  RadioListenPolicy listen_policy_ = RadioListenPolicy::kContinuous;
  RadioListenState listen_state_ = RadioListenState::kRxContinuous;
  uint32_t listen_window_deadline_ms_ = 0;
  bool rx_accounting_active_ = false;
  uint32_t rx_accounting_started_ms_ = 0;
  RadioListenDiagnostics listen_diagnostics_{};
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
