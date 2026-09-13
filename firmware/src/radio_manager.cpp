#include "radio_manager.h"
#include "gnss_config.h"

#include <Arduino.h>
#include <FreeRTOS.h>
#include <SX126x-Arduino.h>
#include <queue.h>
#include <task.h>

#include "gnss_manager.h"
#include "monotonic_time.h"
#include "radio_config.h"
#include "radio_driver_gate.h"
#include "tlp_position_packet.h"
#include "tlp_relay_forward_packet.h"
#include "tlp_test_packet.h"

namespace orun_tlp {
namespace {

RadioManager* active_radio_manager = nullptr;
RadioEvents_t radio_events{};

// SX126x-Arduino 2.0.32 owns a 255-byte receive buffer. Copy the callback
// payload before the library reuses it; accepted TLP frames are currently at
// most 49 bytes, but retaining the driver's actual limit avoids a hidden
// protocol ceiling in this handoff layer.
constexpr uint16_t kRadioReceiveLimit = 255;
constexpr UBaseType_t kRxEventQueueCapacity = 4;

struct RadioRxEvent {
  uint32_t role_epoch = 0;
  uint16_t size = 0;
  int16_t rssi = 0;
  int8_t snr = 0;
  uint8_t payload[kRadioReceiveLimit]{};
};

StaticQueue_t rx_event_queue_control{};
alignas(uint32_t) uint8_t rx_event_queue_storage[
    kRxEventQueueCapacity * sizeof(RadioRxEvent)]{};
QueueHandle_t rx_event_queue = nullptr;

static_assert(tlp::kRelayForwardPacketSize <= kRadioReceiveLimit,
              "largest TLP packet must fit radio event storage");

uint64_t boardUniqueIdToUint64(const uint8_t* board_id) {
  uint64_t device_id = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    device_id = (device_id << 8) | board_id[index];
  }
  return device_id;
}

uint32_t deterministicJitter(uint64_t device_id, uint32_t sequence_number,
                             uint32_t range) {
  uint32_t value = static_cast<uint32_t>(device_id) ^
                   static_cast<uint32_t>(device_id >> 32) ^ sequence_number;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  return value % range;
}

}  // namespace

bool RadioManager::begin(SequenceSource& sequences) {
  // nRF52 BoardGetUniqueId reads factory registers, independent of the radio.
  // Recovery and local POSITION encoding must work even if radio startup fails.
  uint8_t board_id[8]{};
  BoardGetUniqueId(board_id);
  device_id_ = boardUniqueIdToUint64(board_id);
  sequences_ = &sequences;
  if (!radio_driver::initialize()) return false;
  radio_driver::Guard gate;
  if (!gate) return false;
  ready_ = false;
  tx_in_progress_ = false;
  tx_kind_ = TxKind::kNone;
  tx_generation_ = armed_tx_generation_ = pending_tx_generation_ = 0;
  role_transition_pending_ = false;
  accept_rx_events_ = true;
  network_.begin(device_id_, NodeRole::kBase);
  event_diagnostics_ = {};
  role_epoch_ = 0;
  tx_role_epoch_ = 0;
  pending_tx_result_ = TxResult::kNone;
  pending_rx_timeouts_ = 0;
  pending_rx_errors_ = 0;
  rx_restore_state_ = RxRestoreState::kNone;
  rx_event_queue = xQueueCreateStatic(
      kRxEventQueueCapacity, sizeof(RadioRxEvent), rx_event_queue_storage,
      &rx_event_queue_control);
  if (rx_event_queue == nullptr) {
    Serial.println(F("LoRa event queue initialization failed"));
    return false;
  }
  xQueueReset(rx_event_queue);

  if (lora_rak4630_init() != 0) {
    Serial.println(F("LoRa initialization failed"));
    return false;
  }

  radio_events.TxDone = onTxDone;
  radio_events.TxTimeout = onTxTimeout;
  radio_events.RxDone = onRxDone;
  radio_events.RxTimeout = onRxTimeout;
  radio_events.RxError = onRxError;
  Radio.Init(&radio_events);
  Radio.SetChannel(radio_config::kFrequencyHz);
  Radio.SetCustomSyncWord(radio_config::kPrivateSyncWord);
  Radio.SetTxConfig(MODEM_LORA, radio_config::kTxPowerDbm, 0,
                    radio_config::kBandwidth, radio_config::kSpreadingFactor,
                    radio_config::kCodingRate, radio_config::kPreambleLength,
                    !radio_config::kExplicitHeader, radio_config::kCrcEnabled,
                    false, 0, radio_config::kIqInverted,
                    radio_config::kTxTimeoutMs);
  Radio.SetRxConfig(MODEM_LORA, radio_config::kBandwidth,
                    radio_config::kSpreadingFactor, radio_config::kCodingRate,
                    0, radio_config::kPreambleLength, 0,
                    !radio_config::kExplicitHeader, 0,
                    radio_config::kCrcEnabled, false, 0,
                    radio_config::kIqInverted, true);

  const uint16_t applied_sync_word = Radio.GetSyncWord();

  active_radio_manager = this;
  Radio.Rx(0);
  next_tx_at_ms_ = monotonic::nowMs() +
                   deterministicJitter(device_id_, 0,
                                       radio_config::kInitialOffsetRangeMs);
  Serial.printf("RF: %lu Hz TX=%d dBm SF%u BW=%u kHz CR=%u/%u preamble=%u sync=0x%04X\n",
                static_cast<unsigned long>(radio_config::kFrequencyHz),
                radio_config::kTxPowerDbm, radio_config::kSpreadingFactor,
                radio_config::kBandwidthKhz,
                radio_config::kCodingRateNumerator,
                radio_config::kCodingRateDenominator,
                radio_config::kPreambleLength, applied_sync_word);
  ready_ = true;
  return true;
}

void RadioManager::update(bool allow_test_beacon) {
  if (!ready_) {
    return;
  }
  radio_driver::Guard gate;
  if (!gate) return; // No driver read or result consumption before cleanup exit.
  // Drain pending DIO work before deciding that a software deadline won.
  orunRadioDispatchLocked();
  if (tx_in_progress_ && pending_tx_result_ == TxResult::kNone &&
      monotonic::elapsed(monotonic::nowMs(), tx_started_ms_, radio_config::kTxTimeoutMs))
    orunRadioTimeoutLocked();
  processCallbackEvents();
  if (role_transition_pending_ && !tx_in_progress_) {
    applyPendingRole();
    return;
  }
  processReceivedEvents();
  if (!serviceRxRestore()) return;

  const uint32_t now = monotonic::nowMs();
  if (network_.role() == NodeRole::kRelay && !tx_in_progress_) {
    sendDueRelay(now);
  }
  if (allow_test_beacon && radio_config::kTestBeaconEnabled && !tx_in_progress_ &&
      monotonic::reached(now, next_tx_at_ms_)) {
    sendTestPacket();
  }
}

void RadioManager::setRole(NodeRole role) {
  // Re-requesting the still-installed role cancels an unapplied transition.
  // No epoch has changed, so an active forward must retain its completion.
  requested_role_ = role;
  role_transition_pending_ = role != network_.role();
  taskENTER_CRITICAL();
  accept_rx_events_ = !role_transition_pending_;
  taskEXIT_CRITICAL();
}

bool RadioManager::canSend() const {
  return ready_ && !tx_in_progress_ &&
         !role_transition_pending_ &&
         rx_restore_state_ == RxRestoreState::kNone;
}

bool RadioManager::encodePosition(const GnssFix& fix, uint8_t* payload, uint64_t& identity) {
  uint32_t sequence;
  if (!payload || !sequences_ || !sequences_->nextSequence(sequence, identity)) return false;
  sequence_number_ = sequence + 1;
  const tlp::PositionPacket packet{
      device_id_, sequence, fix.utc_epoch_seconds, fix.latitude_e7,
      fix.longitude_e7, fix.altitude_mm, fix.hdop_x100, fix.satellites,
      fix.flags};
  return tlp::serializePositionPacket(packet, payload, tlp::kPositionPacketSize);
}

bool RadioManager::sendPositionPacket(const uint8_t* payload, const uint32_t* captured_at_ms) {
  radio_driver::Guard gate;
  if (!gate) return false;
  tlp::PositionPacket packet{};
  if (!canSend() || !tlp::deserializePositionPacket(payload, tlp::kPositionPacketSize, &packet) ||
      packet.source_device_id != device_id_) {
    ++local_tx_failures_;
    return false;
  }
  // The caller supplies the immutable stored packet, including its sequence.
  // SX126x-Arduino copies these bytes synchronously into its FIFO in Send().
  uint8_t tx_payload[tlp::kPositionPacketSize];
  memcpy(tx_payload, payload, sizeof(tx_payload));
  // Final live admission check under the driver gate. No blocking log between
  // this check and Send; stored/backlog packets deliberately omit this gate.
  if (captured_at_ms && monotonic::elapsed(monotonic::nowMs(), *captured_at_ms,
                                          gnss_config::kFreshFixMaxAgeMs)) return false;
  startTxOperation();
  tx_kind_ = TxKind::kPosition;
  tx_role_epoch_ = role_epoch_;
  rx_restore_state_ = RxRestoreState::kNone;
  ++tx_attempts_;
  Radio.Send(tx_payload, sizeof(tx_payload));
  tx_started_ms_ = monotonic::nowMs(); // Match upstream timer start after Send.
  Serial.printf("TX POSITION source=%016llX sequence=%lu lat=%ld lon=%ld sats=%u\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number),
                static_cast<long>(packet.latitude_e7),
                static_cast<long>(packet.longitude_e7), packet.satellites);
  return true;
}

uint64_t RadioManager::deviceId() const { return device_id_; }

void RadioManager::onTxDone() {
  if (active_radio_manager != nullptr)
    active_radio_manager->postTxResult(TxResult::kDone);
}

void RadioManager::onTxTimeout() {
  if (active_radio_manager != nullptr)
    active_radio_manager->postTxResult(TxResult::kTimeout);
}

void RadioManager::onRxDone(uint8_t* payload, uint16_t size, int16_t rssi,
                            int8_t snr) {
  if (active_radio_manager != nullptr) {
    active_radio_manager->enqueueRxEvent(payload, size, rssi, snr);
  }
}

void RadioManager::onRxTimeout() {
  if (active_radio_manager != nullptr) active_radio_manager->postRxTimeout();
}

void RadioManager::onRxError() {
  if (active_radio_manager != nullptr) active_radio_manager->postRxError();
}

RadioEventDiagnostics RadioManager::eventDiagnostics() const {
  taskENTER_CRITICAL();
  const RadioEventDiagnostics snapshot = event_diagnostics_;
  taskEXIT_CRITICAL();
  return snapshot;
}

void RadioManager::enqueueRxEvent(const uint8_t* payload, uint16_t size,
                                  int16_t rssi, int8_t snr) {
  taskENTER_CRITICAL();
  const bool accepting = accept_rx_events_;
  if (!accepting) ++event_diagnostics_.stale_rx_events;
  taskEXIT_CRITICAL();
  if (!accepting) return;
  if (payload == nullptr || size > kRadioReceiveLimit) {
    taskENTER_CRITICAL();
    ++event_diagnostics_.oversized_rx_drops;
    taskEXIT_CRITICAL();
    return;
  }

  RadioRxEvent event{};
  taskENTER_CRITICAL();
  event.role_epoch = role_epoch_;
  taskEXIT_CRITICAL();
  event.size = size;
  event.rssi = rssi;
  event.snr = snr;
  memcpy(event.payload, payload, size);
  if (rx_event_queue == nullptr ||
      xQueueSend(rx_event_queue, &event, 0) != pdPASS) {
    taskENTER_CRITICAL();
    ++event_diagnostics_.rx_queue_drops;
    taskEXIT_CRITICAL();
  }
}

void RadioManager::postTxResult(TxResult result) {
  const uint32_t generation = radio_driver::generation();
  taskENTER_CRITICAL();
  if (generation == 0 || generation != armed_tx_generation_ ||
      pending_tx_result_ != TxResult::kNone) {
    ++event_diagnostics_.stale_tx_results;
  } else {
    pending_tx_result_ = result;
    pending_tx_generation_ = generation;
  }
  taskEXIT_CRITICAL();
}

void RadioManager::postRxTimeout() {
  taskENTER_CRITICAL();
  if (pending_rx_timeouts_ != UINT32_MAX)
    ++pending_rx_timeouts_;
  else
    ++event_diagnostics_.control_event_drops;
  taskEXIT_CRITICAL();
}

void RadioManager::postRxError() {
  taskENTER_CRITICAL();
  if (pending_rx_errors_ != UINT32_MAX)
    ++pending_rx_errors_;
  else
    ++event_diagnostics_.control_event_drops;
  taskEXIT_CRITICAL();
}

void RadioManager::processCallbackEvents() {
  taskENTER_CRITICAL();
  const TxResult tx_result = pending_tx_result_;
  const uint32_t generation = pending_tx_generation_;
  const uint32_t rx_timeouts = pending_rx_timeouts_;
  const uint32_t rx_errors = pending_rx_errors_;
  pending_tx_result_ = TxResult::kNone;
  pending_rx_timeouts_ = 0;
  pending_rx_errors_ = 0;
  taskEXIT_CRITICAL();

  if (tx_result != TxResult::kNone) {
    if (tx_in_progress_ && generation == tx_generation_) {
      armed_tx_generation_ = 0;
      const bool succeeded = tx_result == TxResult::kDone;
      if (tx_kind_ == TxKind::kRelay && tx_role_epoch_ == role_epoch_ &&
          !role_transition_pending_) {
        network_.onForwardTxResult(succeeded);
        Serial.println(succeeded ? F("RELAY TX done") : F("RELAY TX timeout"));
      }
      if (!succeeded) {
        ++tx_timeouts_;
        Serial.println(F("TX timeout"));
      }
      // Local TX completion is not application delivery confirmation.
      tx_kind_ = TxKind::kNone;
      tx_in_progress_ = false;
      scheduleNextTransmission(monotonic::nowMs());
    } else {
      ++event_diagnostics_.stale_tx_results;
    }
    requestRxRestore();
  }

  if (rx_timeouts != 0) requestRxRestore();
  if (rx_errors != 0) {
    Serial.println(F("RX error (CRC or incomplete packet)"));
    requestRxRestore();
  }
}

void RadioManager::processReceivedEvents() {
  if (rx_event_queue == nullptr) return;
  RadioRxEvent event{};
  for (UBaseType_t n = 0; n < kRxEventQueueCapacity &&
       xQueueReceive(rx_event_queue, &event, 0) == pdPASS; ++n) {
    if (role_transition_pending_ || event.role_epoch != role_epoch_) {
      ++event_diagnostics_.stale_rx_events;
      continue;
    }
    handleReceivedPacket(event.payload, event.size, event.rssi, event.snr);
    requestRxRestore();
  }
}

void RadioManager::requestRxRestore() {
  rx_restore_state_ = RxRestoreState::kRequired;
}

bool RadioManager::serviceRxRestore() {
  if (tx_in_progress_) return false;
  if (rx_restore_state_ == RxRestoreState::kNone) return true;

  // Caller holds the gate across dispatch, result consumption and recovery.
  const RadioState_t state = Radio.GetStatus();
  if (state == RF_TX_RUNNING || state == RF_CAD) return false;
  if (state == RF_RX_RUNNING) {
    rx_restore_state_ = RxRestoreState::kNone;
    return true;
  }
  orunRadioQuiesceLocked();
  Radio.Rx(0);
  rx_restore_state_ = RxRestoreState::kNone;
  return false;
}

void RadioManager::startTxOperation() {
  // No previous TX can be active here. Stop RX and clear pending hardware
  // IRQs before changing the callback identity; a waiting LORA dispatch then
  // observes only empty registers or the newly armed operation.
  orunRadioQuiesceLocked();
  if (++tx_generation_ == 0) ++tx_generation_;
  armed_tx_generation_ = tx_generation_;
  radio_driver::setGeneration(tx_generation_);
  tx_in_progress_ = true;
}

void RadioManager::applyPendingRole() {
  // Callbacks were rejected at request time. Gate excludes even a dispatch
  // paused before its application callback; quiesce removes not-yet-read IRQs.
  orunRadioQuiesceLocked();
  processReceivedEvents();
  network_.setRole(requested_role_);
  ++role_epoch_;
  armed_tx_generation_ = 0;
  radio_driver::setGeneration(0);
  role_transition_pending_ = false;
  rx_restore_state_ = RxRestoreState::kNone;
  Radio.Rx(0);
  taskENTER_CRITICAL();
  accept_rx_events_ = true;
  taskEXIT_CRITICAL();
}

void RadioManager::sendTestPacket() {
  uint8_t payload[tlp::kTestPacketSize]{};
  uint32_t sequence;
  uint64_t identity;
  if (!sequences_ || !sequences_->nextSequence(sequence, identity)) return;
  sequence_number_ = sequence + 1;
  const tlp::TestPacket packet{device_id_, sequence, monotonic::nowMs()};
  if (!tlp::serializeTestPacket(packet, payload, sizeof(payload))) {
    Serial.println(F("TEST packet serialization failed"));
    scheduleNextTransmission(monotonic::nowMs());
    return;
  }

  startTxOperation();
  tx_kind_ = TxKind::kTest;
  tx_role_epoch_ = role_epoch_;
  rx_restore_state_ = RxRestoreState::kNone;
  ++tx_attempts_;
  Serial.printf("TX source=%016llX sequence=%lu type=TEST\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number));
  Radio.Send(payload, sizeof(payload));
  tx_started_ms_ = monotonic::nowMs();
}

void RadioManager::sendDueRelay(uint32_t now) {
  tlp::RelayForwardPacket envelope{};
  if (!network_.takeDueForward(now, &envelope)) return;
  uint8_t payload[tlp::kRelayForwardPacketSize]{};
  if (!tlp::serializeRelayForwardPacket(envelope, payload, sizeof(payload))) {
    network_.onForwardTxResult(false);
    ++local_tx_failures_;
    Serial.println(F("RELAY TX encode failed"));
    return;
  }
  tlp::PositionPacket original{};
  if (!tlp::deserializePositionPacket(envelope.original_packet,
                                      envelope.original_length, &original)) {
    network_.onForwardTxResult(false);
    ++local_tx_failures_;
    Serial.println(F("RELAY TX inner validation failed"));
    return;
  }
  startTxOperation();
  tx_kind_ = TxKind::kRelay;
  tx_role_epoch_ = role_epoch_;
  rx_restore_state_ = RxRestoreState::kNone;
  ++tx_attempts_;
  network_.onForwardTxStarted();
  Serial.printf("RELAY TX source=%016llX seq=%lu\n",
                static_cast<unsigned long long>(original.source_device_id),
                static_cast<unsigned long>(original.sequence_number));
  Radio.Send(payload, sizeof(payload));
  tx_started_ms_ = monotonic::nowMs();
}

void RadioManager::scheduleNextTransmission(uint32_t now) {
  next_tx_at_ms_ = now + radio_config::kTestIntervalMs +
                   deterministicJitter(device_id_, sequence_number_,
                                       radio_config::kPerPacketJitterRangeMs);
}

void RadioManager::handleReceivedPacket(const uint8_t* payload, uint16_t size,
                                        int16_t rssi, int8_t snr) {
  if (size >= 2 && payload[0] == tlp::kProtocolVersion &&
      payload[1] == tlp::kPacketTypeTest) {
    tlp::TestPacket packet{};
    if (!tlp::deserializeTestPacket(payload, size, &packet)) {
      Serial.printf("RX TEST rejected length=%u\n", size);
      return;
    }
    Serial.printf("RX source=%016llX sequence=%lu type=TEST RSSI=%d dBm SNR=%d dB\n",
                  static_cast<unsigned long long>(packet.source_device_id),
                  static_cast<unsigned long>(packet.sequence_number), rssi, snr);
    return;
  }

  const NetworkEvent event =
      network_.receive(payload, size, rssi, snr, monotonic::nowMs());
  const auto source = static_cast<unsigned long long>(event.position.source_device_id);
  const auto sequence = static_cast<unsigned long>(event.position.sequence_number);
  switch (event.kind) {
    case NetworkEventKind::kRelayQueued:
      Serial.printf("RELAY RX source=%016llX seq=%lu rssi=%d snr=%d\n",
                    source, sequence, rssi, snr);
      Serial.printf("RELAY QUEUE source=%016llX seq=%lu delay=%lums\n",
                    source, sequence,
                    static_cast<unsigned long>(event.relay_delay_ms));
      break;
    case NetworkEventKind::kRelayDuplicate:
      Serial.printf("RELAY DUP source=%016llX seq=%lu\n", source, sequence);
      break;
    case NetworkEventKind::kRelayQueueDrop:
      Serial.printf("RELAY DROP queue-full source=%016llX seq=%lu\n",
                    source, sequence);
      break;
    case NetworkEventKind::kRelayNestedRejected:
      Serial.println(F("RELAY rejected relayed packet"));
      break;
    case NetworkEventKind::kBaseNew:
    case NetworkEventKind::kBaseDuplicate: {
      const char* freshness = event.kind == NetworkEventKind::kBaseNew ? "NEW" : "DUP";
      if (event.path == NetworkPath::kDirect) {
        Serial.printf("BASE RX %s source=%016llX seq=%lu path=DIRECT rssi=%d snr=%d\n",
                      freshness, source, sequence, event.link_rssi_dbm,
                      event.link_snr_db);
      } else {
        Serial.printf("BASE RX %s source=%016llX seq=%lu path=RELAY relay=%016llX ingress_rssi=%d ingress_snr=%d rssi=%d snr=%d\n",
                      freshness, source, sequence,
                      static_cast<unsigned long long>(event.relay_device_id),
                      event.ingress_rssi_dbm, event.ingress_snr_db,
                      event.link_rssi_dbm, event.link_snr_db);
      }
      break;
    }
    case NetworkEventKind::kIgnoredPosition:
      Serial.printf("RX POSITION source=%016llX seq=%lu ignored role=%s\n",
                    source, sequence, roleName(network_.role()));
      break;
    case NetworkEventKind::kMalformed:
      Serial.printf("RX rejected type=%u length=%u role=%s\n",
                    size >= 2 ? payload[1] : 0, size,
                    roleName(network_.role()));
      break;
    case NetworkEventKind::kNone:
      break;
  }
}

}  // namespace orun_tlp
