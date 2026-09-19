#pragma once
// Host stub: only the minimal Bluefruit surface main.cpp actually calls.
// The startup/integration test drives main.cpp's BLE boot path, runtime
// admission loop and BLE? diagnostic against this stub, modelling pinned
// Adafruit nRF52 1.7.0:
//  - BLEAdvertising::start()/stop()/isRunning(): start_result/stop_result
//    are test knobs; a failed start()/stop() leaves _running unchanged.
//  - BLEPeriph::connected(): connected_count.
//  - simulateConnect(): BLE_GAP_EVT_CONNECTED sets _running=false.
//  - simulateDisconnect(): BLE_GAP_EVT_DISCONNECTED restarts advertising
//    (result ignored, like the framework) ONLY when restartOnDisconnect is
//    enabled. Then, in the order of AdafruitBluefruit::_ble_handler():
//      1. Periph's disconnect callback is QUEUED the way ada_callback() does
//         (it runs later, on another task, via deliverPendingCallbacks(), and
//         is droppable -- see Periph.drop_callbacks);
//      2. the global Bluefruit.setEventCallback() callback, if registered, is
//         called DIRECTLY and synchronously, last, after connection state
//         was already updated. Nothing is queued or allocated on that path.
//    simulateConnect() likewise delivers BLE_GAP_EVT_CONNECTED to the global
//    callback. event_delivery=false models "the event never reaches the
//    callback" for mutation checks.
// The pure admission policy is also host-tested directly, with no Bluefruit
// dependency, by firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp -- see
// docs/milestones/M7P7B.md.
#include <stdint.h>

// Only the fields main.cpp reads, at the real SoftDevice s140 7.3.0 values
// (ble.h ble_evt_hdr_t::evt_id; ble_gap.h BLE_GAP_EVT_BASE = 0x10).
struct ble_evt_t {
  struct {
    uint16_t evt_id;
  } header;
};
constexpr uint16_t BLE_GAP_EVT_CONNECTED = 0x10;
constexpr uint16_t BLE_GAP_EVT_DISCONNECTED = 0x11;

struct BleAdvertisingStub {
  bool start_result = true;  // Test knob: what start() reports.
  bool stop_result = true;   // Test knob: what stop() reports.
  // Test hook run inside stop() before it reports failure: models a
  // connection winning the race with sd_ble_gap_adv_stop().
  void (*stop_race)() = nullptr;
  bool running = false;
  bool restart_on_disconnect = true;  // Framework default (_start_if_disconnect).
  unsigned start_calls = 0;
  unsigned stop_calls = 0;
  bool start(uint16_t = 0) {
    ++start_calls;
    if (start_result) running = true;  // Failed start() leaves _running as-is.
    return start_result;
  }
  bool isRunning() const { return running; }
  bool stop() {
    ++stop_calls;
    if (stop_race != nullptr) {
      stop_race();
      return false;
    }
    if (!stop_result) return false;  // _running unchanged, like the framework.
    running = false;
    return true;
  }
  void restartOnDisconnect(bool enable) { restart_on_disconnect = enable; }
  bool addFlags(uint8_t) { return true; }
  bool addName() { return true; }
};

// Real value from the pinned SoftDevice's ble_gap.h; only used as an opaque
// argument to the stub above.
constexpr uint8_t BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE = 0x06;

struct BlePeriphStub {
  uint8_t connected_count = 0;  // Test knob.
  void (*disconnect_cb)(uint16_t, uint8_t) = nullptr;
  unsigned pending_disconnect_cbs = 0;  // Queued, not yet run (ada_callback).
  bool drop_callbacks = false;  // Test knob: ada_callback_invoke() malloc failure.
  uint8_t connected() { return connected_count; }
  void setDisconnectCallback(void (*fp)(uint16_t, uint8_t)) { disconnect_cb = fp; }
};

struct AdafruitBluefruitStub {
  BleAdvertisingStub Advertising;
  BlePeriphStub Periph;
  bool begin_result = true;  // Test knob: what begin() reports.
  void (*event_cb)(ble_evt_t*) = nullptr;  // Bluefruit.setEventCallback().
  bool event_delivery = true;  // Test knob: false = callback never invoked.
  unsigned event_cb_calls = 0;
  void setEventCallback(void (*fp)(ble_evt_t*)) { event_cb = fp; }
  bool begin(uint8_t = 1, uint8_t = 0) { return begin_result; }
  void setName(const char*) {}
  void autoConnLed(bool) {}
  // Framework transitions (BLE_GAP_EVT_CONNECTED / _DISCONNECTED in
  // BLEAdvertising::_eventHandler); not part of main.cpp's call surface.
  void dispatchEvent(uint16_t evt_id) {
    if (event_cb == nullptr || !event_delivery) return;
    ble_evt_t evt{};
    evt.header.evt_id = evt_id;
    ++event_cb_calls;
    event_cb(&evt);
  }
  void simulateConnect() {
    Periph.connected_count = 1;
    Advertising.running = false;
    dispatchEvent(BLE_GAP_EVT_CONNECTED);
  }
  void simulateDisconnect() {
    Periph.connected_count = 0;
    if (Advertising.restart_on_disconnect) Advertising.running = Advertising.start_result;
    if (Periph.disconnect_cb != nullptr && !Periph.drop_callbacks) {
      ++Periph.pending_disconnect_cbs;
    }
    dispatchEvent(BLE_GAP_EVT_DISCONNECTED);
  }
  // The "Callback" task getting scheduled: runs every queued disconnect
  // callback, off the loop task's flow.
  void deliverPendingCallbacks() {
    while (Periph.pending_disconnect_cbs > 0) {
      --Periph.pending_disconnect_cbs;
      Periph.disconnect_cb(0, 0x13);
    }
  }
};

inline AdafruitBluefruitStub Bluefruit;
