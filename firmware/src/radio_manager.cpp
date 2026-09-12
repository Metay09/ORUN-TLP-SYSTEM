#include "radio_manager.h"

#include <Arduino.h>
#include <SX126x-Arduino.h>

#include "radio_config.h"
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

bool RadioManager::begin() {
  uint8_t board_id[8]{};
  BoardGetUniqueId(board_id);
  device_id_ = boardUniqueIdToUint64(board_id);

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

void RadioManager::update() {
  if (!ready_) {
    return;
  }
  const uint32_t now = millis();
  if (!tx_in_progress_ && static_cast<int32_t>(now - next_tx_at_ms_) >= 0) {
    sendTestPacket();
  }
}

uint64_t RadioManager::deviceId() const { return device_id_; }

void RadioManager::onTxDone() {
  if (active_radio_manager != nullptr) {
    active_radio_manager->tx_in_progress_ = false;
    active_radio_manager->scheduleNextTransmission(millis());
  }
  Radio.Rx(0);
}

void RadioManager::onTxTimeout() {
  Serial.println(F("TX timeout"));
  if (active_radio_manager != nullptr) {
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
  const tlp::TestPacket packet{device_id_, sequence_number_++, millis()};
  if (!tlp::serializeTestPacket(packet, payload, sizeof(payload))) {
    Serial.println(F("TEST packet serialization failed"));
    scheduleNextTransmission(millis());
    return;
  }

  tx_in_progress_ = true;
  Serial.printf("TX source=%016llX sequence=%lu type=TEST\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number));
  Radio.Send(payload, sizeof(payload));
}

void RadioManager::scheduleNextTransmission(uint32_t now) {
  next_tx_at_ms_ = now + radio_config::kTestIntervalMs +
                   deterministicJitter(device_id_, sequence_number_,
                                       radio_config::kPerPacketJitterRangeMs);
}

void RadioManager::handleReceivedPacket(const uint8_t* payload, uint16_t size,
                                        int16_t rssi, int8_t snr) {
  tlp::TestPacket packet{};
  if (!tlp::deserializeTestPacket(payload, size, &packet)) {
    Serial.printf("RX rejected: length=%u or unsupported version/type\n", size);
    return;
  }

  Serial.printf("RX source=%016llX sequence=%lu type=TEST RSSI=%d dBm SNR=%d dB\n",
                static_cast<unsigned long long>(packet.source_device_id),
                static_cast<unsigned long>(packet.sequence_number), rssi, snr);
}

}  // namespace orun_tlp
