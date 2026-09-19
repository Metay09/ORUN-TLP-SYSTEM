#pragma once
// Host stub: only the minimal Bluefruit surface main.cpp actually calls.
// This startup/integration test exercises radio/gate/GNSS/PositionFlow/
// journal boot behavior against host stubs; it does not assert anything
// about BLE state. The actual BLE admission policy is host-tested directly,
// with no Bluefruit dependency, by
// firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp -- see
// docs/milestones/M7P7B.md.
#include <stdint.h>

struct BleAdvertisingStub {
  bool start(uint16_t = 0) { return true; }
  bool stop() { return true; }
  void restartOnDisconnect(bool) {}
  bool addFlags(uint8_t) { return true; }
  bool addName() { return true; }
};

// Real value from the pinned SoftDevice's ble_gap.h; only used as an opaque
// argument to the stub above.
constexpr uint8_t BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE = 0x06;

struct BlePeriphStub {
  uint8_t connected() { return 0; }
};

struct AdafruitBluefruitStub {
  BleAdvertisingStub Advertising;
  BlePeriphStub Periph;
  bool begin(uint8_t = 1, uint8_t = 0) { return true; }
  void setName(const char*) {}
  void autoConnLed(bool) {}
};

inline AdafruitBluefruitStub Bluefruit;
