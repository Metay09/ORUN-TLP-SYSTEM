#pragma once

#include <stdint.h>

namespace orun_tlp {

struct GnssFix;

class RadioManager {
 public:
  bool begin();
  void update();
  bool canSend() const;
  bool sendPosition(const GnssFix& fix);
  uint64_t deviceId() const;

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

  uint64_t device_id_ = 0;
  uint32_t sequence_number_ = 0;
  uint32_t next_tx_at_ms_ = 0;
  bool ready_ = false;
  volatile bool tx_in_progress_ = false;
};

}  // namespace orun_tlp
