#pragma once
// Host stub: only the minimal Bluefruit surface main.cpp actually calls.
// The startup/integration test drives main.cpp's BLE boot path and BLE?
// diagnostic against this stub: start_result/running model
// BLEAdvertising::start()/isRunning() (Adafruit nRF52 1.7.0), and
// connected_count models BLEPeriph::connected(). simulateConnect()/
// simulateDisconnect() mirror BLEAdvertising::_eventHandler(): a connection
// sets _running=false; a disconnect auto-restarts advertising when
// restartOnDisconnect is enabled (framework default true). The pure admission policy
// is also host-tested directly, with no Bluefruit dependency, by
// firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp -- see
// docs/milestones/M7P7B.md.
#include <stdint.h>

struct BleAdvertisingStub {
  bool start_result = true;  // Test knob: what start() reports.
  bool running = false;
  bool restart_on_disconnect = true;  // Framework default (_start_if_disconnect).
  unsigned start_calls = 0;
  bool start(uint16_t = 0) {
    ++start_calls;
    running = start_result;
    return start_result;
  }
  bool isRunning() const { return running; }
  bool stop() { running = false; return true; }
  void restartOnDisconnect(bool enable) { restart_on_disconnect = enable; }
  bool addFlags(uint8_t) { return true; }
  bool addName() { return true; }
};

// Real value from the pinned SoftDevice's ble_gap.h; only used as an opaque
// argument to the stub above.
constexpr uint8_t BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE = 0x06;

struct BlePeriphStub {
  uint8_t connected_count = 0;  // Test knob.
  uint8_t connected() { return connected_count; }
};

struct AdafruitBluefruitStub {
  BleAdvertisingStub Advertising;
  BlePeriphStub Periph;
  bool begin_result = true;  // Test knob: what begin() reports.
  bool begin(uint8_t = 1, uint8_t = 0) { return begin_result; }
  void setName(const char*) {}
  void autoConnLed(bool) {}
  // Framework transitions (BLE_GAP_EVT_CONNECTED / _DISCONNECTED in
  // BLEAdvertising::_eventHandler); not part of main.cpp's call surface.
  void simulateConnect() {
    Periph.connected_count = 1;
    Advertising.running = false;
  }
  void simulateDisconnect() {
    Periph.connected_count = 0;
    if (Advertising.restart_on_disconnect) Advertising.running = true;
  }
};

inline AdafruitBluefruitStub Bluefruit;
