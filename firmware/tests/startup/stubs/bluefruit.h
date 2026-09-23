#pragma once
// Host stub for the exact Bluefruit surface production main.cpp exercises.
// It models M7P7B admission plus M7P7G's bounded GATT setup/callback/HVC seam.
// Bluetooth timing/security remain production-build and physical-test scope.

#include <stdint.h>
#include <string.h>

using SecureMode_t = uint8_t;
constexpr SecureMode_t SECMODE_NO_ACCESS = 0;
constexpr SecureMode_t SECMODE_OPEN = 1;

constexpr uint32_t ERROR_NONE = 0;
constexpr uint16_t BLE_GATT_HANDLE_INVALID = 0x0000;
constexpr uint16_t BLE_CONN_HANDLE_INVALID = 0xFFFF;
constexpr uint8_t BLE_GATT_HVX_INDICATION = 0x02;
constexpr uint8_t CHR_PROPS_WRITE = 0x08;
constexpr uint8_t CHR_PROPS_INDICATE = 0x20;

constexpr uint16_t BLE_GAP_EVT_CONNECTED = 0x10;
constexpr uint16_t BLE_GAP_EVT_DISCONNECTED = 0x11;
constexpr uint16_t BLE_GATTS_EVT_HVC = 0x55;
constexpr uint16_t BLE_GATTS_EVT_TIMEOUT = 0x56;
constexpr uint8_t BLE_GATT_TIMEOUT_SRC_PROTOCOL = 0x00;

struct ble_evt_t {
  struct {
    uint16_t evt_id = 0;
  } header;
  struct {
    struct {
      uint16_t conn_handle = BLE_CONN_HANDLE_INVALID;
    } common_evt;
    struct {
      uint16_t conn_handle = BLE_CONN_HANDLE_INVALID;
      struct {
        struct {
          uint16_t handle = BLE_GATT_HANDLE_INVALID;
        } hvc;
        struct {
          uint8_t src = 0xFF;
        } timeout;
      } params;
    } gatts_evt;
  } evt;
};

struct ble_gatts_hvx_params_t {
  uint16_t handle = BLE_GATT_HANDLE_INVALID;
  uint8_t type = 0;
  uint16_t offset = 0;
  uint16_t* p_len = nullptr;
  uint8_t* p_data = nullptr;
};

struct ble_gatts_char_handles_t {
  uint16_t value_handle = BLE_GATT_HANDLE_INVALID;
  uint16_t user_desc_handle = BLE_GATT_HANDLE_INVALID;
  uint16_t sccd_handle = BLE_GATT_HANDLE_INVALID;
  uint16_t cccd_handle = BLE_GATT_HANDLE_INVALID;
};

class BLEUuid {
 public:
  BLEUuid() = default;
  explicit BLEUuid(const uint8_t*) {}
};

class BLEService {
 public:
  void setUuid(BLEUuid) {}
  uint32_t begin() {
    begun = true;
    return ERROR_NONE;
  }
  bool begun = false;
};

class BLECharacteristic {
 public:
  using write_cb_t =
      void (*)(uint16_t, BLECharacteristic*, uint8_t*, uint16_t);

  void setUuid(BLEUuid) {}
  void setProperties(uint8_t value) { properties = value; }
  void setPermission(SecureMode_t read, SecureMode_t write) {
    read_permission = read;
    write_permission = write;
  }
  void setMaxLen(uint16_t value) { max_len = value; }
  void setWriteCallback(write_cb_t cb, bool use_ada_callback = true) {
    write_callback = cb;
    use_ada = use_ada_callback;
  }
  uint32_t begin() {
    begun = true;
    handles_.value_handle = next_value_handle++;
    if (properties & CHR_PROPS_INDICATE)
      handles_.cccd_handle = next_value_handle++;
    return ERROR_NONE;
  }
  ble_gatts_char_handles_t handles() const { return handles_; }
  bool indicateEnabled(uint16_t) const { return indicate_enabled; }

  void simulateWrite(uint16_t conn_handle, const uint8_t* data, uint16_t len) {
    if (write_callback != nullptr)
      write_callback(conn_handle, this, const_cast<uint8_t*>(data), len);
  }

  uint8_t properties = 0;
  uint16_t max_len = 0;
  SecureMode_t read_permission = SECMODE_OPEN;
  SecureMode_t write_permission = SECMODE_OPEN;
  write_cb_t write_callback = nullptr;
  bool use_ada = true;
  bool indicate_enabled = false;
  bool begun = false;

 private:
  inline static uint16_t next_value_handle = 0x0100;
  ble_gatts_char_handles_t handles_{};
};

struct BleHvxStub {
  uint32_t result = 0;
  unsigned calls = 0;
  uint16_t conn_handle = BLE_CONN_HANDLE_INVALID;
  uint16_t value_handle = BLE_GATT_HANDLE_INVALID;
  uint8_t type = 0;
  uint16_t len = 0;
  uint8_t data[20]{};
};

inline BleHvxStub BluefruitHvx;

inline uint32_t sd_ble_gatts_hvx(uint16_t conn_handle,
                                 ble_gatts_hvx_params_t* params) {
  ++BluefruitHvx.calls;
  BluefruitHvx.conn_handle = conn_handle;
  if (params != nullptr) {
    BluefruitHvx.value_handle = params->handle;
    BluefruitHvx.type = params->type;
    const uint16_t requested = params->p_len != nullptr ? *params->p_len : 0;
    BluefruitHvx.len =
        requested > sizeof(BluefruitHvx.data) ? sizeof(BluefruitHvx.data)
                                              : requested;
    if (BluefruitHvx.len > 0 && params->p_data != nullptr)
      memcpy(BluefruitHvx.data, params->p_data, BluefruitHvx.len);
  }
  return BluefruitHvx.result;
}

struct BleAdvertisingStub {
  bool start_result = true;
  bool stop_result = true;
  void (*stop_race)() = nullptr;
  bool running = false;
  bool restart_on_disconnect = true;
  unsigned start_calls = 0;
  unsigned stop_calls = 0;
  bool start(uint16_t = 0) {
    ++start_calls;
    if (start_result) running = true;
    return start_result;
  }
  bool isRunning() const { return running; }
  bool stop() {
    ++stop_calls;
    if (stop_race != nullptr) {
      stop_race();
      return false;
    }
    if (!stop_result) return false;
    running = false;
    return true;
  }
  void restartOnDisconnect(bool enable) { restart_on_disconnect = enable; }
  bool addFlags(uint8_t) { return true; }
  bool addName() { return true; }
};

constexpr uint8_t BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE = 0x06;

struct BlePeriphStub {
  uint8_t connected_count = 0;
  void (*disconnect_cb)(uint16_t, uint8_t) = nullptr;
  unsigned pending_disconnect_cbs = 0;
  bool drop_callbacks = false;
  uint8_t connected() const { return connected_count; }
  void setDisconnectCallback(void (*fp)(uint16_t, uint8_t)) {
    disconnect_cb = fp;
  }
};

struct AdafruitBluefruitStub {
  BleAdvertisingStub Advertising;
  BlePeriphStub Periph;
  bool begin_result = true;
  void (*event_cb)(ble_evt_t*) = nullptr;
  bool event_delivery = true;
  unsigned event_cb_calls = 0;
  uint16_t active_conn_handle = BLE_CONN_HANDLE_INVALID;
  bool disconnect_result = true;
  unsigned disconnect_calls = 0;

  void setEventCallback(void (*fp)(ble_evt_t*)) { event_cb = fp; }
  bool begin(uint8_t = 1, uint8_t = 0) { return begin_result; }
  void setName(const char*) {}
  void autoConnLed(bool) {}
  uint16_t connHandle() const { return active_conn_handle; }

  bool disconnect(uint16_t conn_handle) {
    ++disconnect_calls;
    if (!disconnect_result || conn_handle != active_conn_handle ||
        Periph.connected_count == 0)
      return false;
    simulateDisconnect();
    return true;
  }

  void dispatchEvent(uint16_t evt_id,
                     uint16_t event_conn_handle = BLE_CONN_HANDLE_INVALID,
                     uint16_t hvc_handle = BLE_GATT_HANDLE_INVALID,
                     uint8_t timeout_src = 0xFF) {
    if (event_cb == nullptr || !event_delivery) return;
    ble_evt_t evt{};
    evt.header.evt_id = evt_id;
    evt.evt.common_evt.conn_handle = event_conn_handle;
    evt.evt.gatts_evt.conn_handle = event_conn_handle;
    evt.evt.gatts_evt.params.hvc.handle = hvc_handle;
    evt.evt.gatts_evt.params.timeout.src = timeout_src;
    ++event_cb_calls;
    event_cb(&evt);
  }

  void simulateConnect() {
    active_conn_handle = 0;
    Periph.connected_count = 1;
    Advertising.running = false;
    dispatchEvent(BLE_GAP_EVT_CONNECTED, active_conn_handle);
  }

  void simulateDisconnect() {
    const uint16_t old_handle = active_conn_handle;
    Periph.connected_count = 0;
    active_conn_handle = BLE_CONN_HANDLE_INVALID;
    if (Advertising.restart_on_disconnect)
      Advertising.running = Advertising.start_result;
    if (Periph.disconnect_cb != nullptr && !Periph.drop_callbacks)
      ++Periph.pending_disconnect_cbs;
    dispatchEvent(BLE_GAP_EVT_DISCONNECTED, old_handle);
  }

  void simulateHvc(uint16_t value_handle) {
    dispatchEvent(BLE_GATTS_EVT_HVC, active_conn_handle, value_handle);
  }

  void simulateGattTimeout() {
    dispatchEvent(BLE_GATTS_EVT_TIMEOUT, active_conn_handle,
                  BLE_GATT_HANDLE_INVALID, BLE_GATT_TIMEOUT_SRC_PROTOCOL);
  }

  void deliverPendingCallbacks() {
    while (Periph.pending_disconnect_cbs > 0) {
      --Periph.pending_disconnect_cbs;
      Periph.disconnect_cb(0, 0x13);
    }
  }
};

inline AdafruitBluefruitStub Bluefruit;
