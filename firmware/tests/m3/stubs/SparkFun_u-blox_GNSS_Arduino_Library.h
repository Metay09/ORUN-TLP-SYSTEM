#pragma once

#include <stdint.h>
#include <functional>
#include <utility>
#include <vector>
#include "Wire.h"
#include "gnss_utc.h"

// Callback/transport seam only. This does NOT emulate UBX parsing or hardware.
struct UBX_NAV_PVT_data_t {
  uint32_t iTOW = 0;
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, min = 0, sec = 0;
  int32_t lat = 0, lon = 0, height = 0;
  uint8_t numSV = 0, fixType = 0;
  struct { struct { bool gnssFixOK = false; } bits; } flags;
  struct { struct { bool invalidLlh = false; } bits; } flags3;
  struct { struct { bool validDate = false, validTime = false, fullyResolved = false; } bits; } valid;
};
struct UBX_NAV_DOP_data_t { uint32_t iTOW; uint16_t hDOP; };
constexpr uint8_t COM_TYPE_UBX = 1;

struct SFE_UBLOX_GNSS {
  inline static bool present = true, configuration_ok = true;
  inline static unsigned reads = 0, config_calls = 0, detection_calls = 0;
  inline static void (*pvt)(UBX_NAV_PVT_data_t*) = nullptr;
  inline static void (*dop)(UBX_NAV_DOP_data_t*) = nullptr;
  inline static std::vector<std::function<void()>> pending;
  bool begin(TwoWire&, uint8_t, uint16_t) { ++detection_calls; return present; }
  bool configure() { ++config_calls; return configuration_ok; }
  bool setI2COutput(uint8_t, uint16_t) { return configure(); }
  bool setNavigationFrequency(uint8_t, uint16_t) { return configure(); }
  bool powerSaveMode(bool, uint16_t) { return configure(); }
  bool setAutoPVTcallbackPtr(void (*fn)(UBX_NAV_PVT_data_t*), uint16_t) {
    pvt = fn; return configure();
  }
  bool setAutoDOPcallbackPtr(void (*fn)(UBX_NAV_DOP_data_t*), uint16_t) {
    dop = fn; return configure();
  }
  inline static bool read_ok = true;
  inline static uint8_t polling_wait = 100, last_read_polling_wait = 100;
  void setI2CpollingWait(uint8_t wait) { polling_wait = wait; }
  bool checkUblox() { ++reads; last_read_polling_wait = polling_wait; return read_ok; }
  void checkCallbacks() {
    auto callbacks = std::move(pending);
    pending.clear();
    for (auto& callback : callbacks) callback();
    if (callback_valid) { pvt(&callback_pvt); callback_valid = false; }
  }
  // 2.2.29 keeps the first unconsumed callback copy while data keeps changing.
  inline static UBX_NAV_PVT_data_t current_pvt{}, callback_pvt{};
  inline static bool callback_valid = false;
  inline static bool itow_fresh = false;
  inline static unsigned time_of_week_cache_misses = 0;
  static void parsePvt(const UBX_NAV_PVT_data_t& value) {
    current_pvt = value;
    itow_fresh = true;
    if (!callback_valid) { callback_pvt = value; callback_valid = true; }
  }
  // Models getTimeOfWeek(0) reading the newest parsed current PVT cache. A
  // cache miss is counted so R3 tests can prove the backlog guard used the
  // fresh parse result rather than relying on a getter-triggered refresh.
  uint32_t getTimeOfWeek(uint16_t) {
    if (!itow_fresh) ++time_of_week_cache_misses;
    itow_fresh = false;
    return current_pvt.iTOW;
  }
  // Models the inspected current-cache date path (all test dates valid).
  uint32_t getUnixEpoch(uint16_t) {
    uint32_t epoch = 0;
    orun_tlp::utcToEpoch({current_pvt.year, current_pvt.month, current_pvt.day,
                        current_pvt.hour, current_pvt.min, current_pvt.sec}, epoch);
    return epoch;
  }
};
