#include "radio_manager.h"

#include <Arduino.h>
#include <SX126x-Arduino.h>

#include "radio_config.h"
#include "gnss_manager.h"
#include "tlp_position_packet.h"
#include "tlp_relay_forward_packet.h"
#include "tlp_test_packet.h"

namespace orun_tlp {
namespace {

RadioManager* active_radio_manager = nullptr;
RadioEvents_t radio_events{};

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
  sequences_ = &sequences;
  uint8_t board_id[8]{};
  BoardGetUniqueId(board_id);
  device_id_ = boardUniqueIdToUint64(board_id);
  network_.begin(device_id_, NodeRole::kBase);

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
  next_tx_at_ms_ = millis() +
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
  const uint32_t now = millis();
  if (network_.role() == NodeRole::kRelay && !tx_in_progress_) {
    sendDueRelay(now);
  }
  if (allow_test_beacon && radio_config::kTestBeaconEnabled && !tx_in_progress_ &&
      static_cast<int32_t>(now - next_tx_at_ms_) >= 0) {
    sendTestPacket();
  }
}

void RadioManager::setRole(NodeRole role) {
  network_.setRole(role);
  if (ready_ && !tx_in_progress_) Radio.Rx(0);
}

bool RadioManager::canSend() const { return ready_ && !tx_in_progress_; }

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

bool RadioManager::sendPositionPacket(const uint8_t* payload) {
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
  tx_in_progress_ = true;
  tx_kind_ = TxKind::kPosition;
  ++tx_attempts_;
  Serial.printf("TX POSITION source=%016llX sequence=%lu lat=%ld lon=%ld sats=%u\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number),
                static_cast<long>(packet.latitude_e7),
                static_cast<long>(packet.longitude_e7), packet.satellites);
  Radio.Send(tx_payload, sizeof(tx_payload));
  return true;
}

uint64_t RadioManager::deviceId() const { return device_id_; }

void RadioManager::onTxDone() {
  if (active_radio_manager != nullptr) {
    // Local TX completion only. No HistoryStore/delivery mutation here.
    if (active_radio_manager->tx_kind_ == TxKind::kRelay) {
      active_radio_manager->network_.onForwardTxResult(true);
      Serial.println(F("RELAY TX done; RX resumed"));
    }
    active_radio_manager->tx_kind_ = TxKind::kNone;
    active_radio_manager->tx_in_progress_ = false;
    active_radio_manager->scheduleNextTransmission(millis());
  }
  Radio.Rx(0);
}

void RadioManager::onTxTimeout() {
  Serial.println(F("TX timeout"));
  if (active_radio_manager != nullptr) {
    if (active_radio_manager->tx_kind_ == TxKind::kRelay) {
      active_radio_manager->network_.onForwardTxResult(false);
      Serial.println(F("RELAY TX timeout; RX resumed"));
    }
    active_radio_manager->tx_kind_ = TxKind::kNone;
    ++active_radio_manager->tx_timeouts_;
    active_radio_manager->tx_in_progress_ = false;
    active_radio_manager->scheduleNextTransmission(millis());
  }
  Radio.Rx(0);
}

void RadioManager::onRxDone(uint8_t* payload, uint16_t size, int16_t rssi,
                            int8_t snr) {
  if (active_radio_manager != nullptr) {
    active_radio_manager->handleReceivedPacket(payload, size, rssi, snr);
  }
  Radio.Rx(0);
}

void RadioManager::onRxTimeout() { Radio.Rx(0); }

void RadioManager::onRxError() {
  Serial.println(F("RX error (CRC or incomplete packet)"));
  Radio.Rx(0);
}

void RadioManager::sendTestPacket() {
  uint8_t payload[tlp::kTestPacketSize]{};
  uint32_t sequence;
  uint64_t identity;
  if (!sequences_ || !sequences_->nextSequence(sequence, identity)) return;
  sequence_number_ = sequence + 1;
  const tlp::TestPacket packet{device_id_, sequence, millis()};
  if (!tlp::serializeTestPacket(packet, payload, sizeof(payload))) {
    Serial.println(F("TEST packet serialization failed"));
    scheduleNextTransmission(millis());
    return;
  }

  tx_in_progress_ = true;
  tx_kind_ = TxKind::kTest;
  ++tx_attempts_;
  Serial.printf("TX source=%016llX sequence=%lu type=TEST\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number));
  Radio.Send(payload, sizeof(payload));
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
  tx_in_progress_ = true;
  tx_kind_ = TxKind::kRelay;
  ++tx_attempts_;
  network_.onForwardTxStarted();
  Serial.printf("RELAY TX source=%016llX seq=%lu\n",
                static_cast<unsigned long long>(original.source_device_id),
                static_cast<unsigned long>(original.sequence_number));
  Radio.Send(payload, sizeof(payload));
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

  const NetworkEvent event = network_.receive(payload, size, rssi, snr, millis());
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
