#pragma once

#include <stdint.h>
#include <functional>
#include <utility>
#include <vector>
#include "Wire.h"

// Callback/transport seam only. This does NOT emulate UBX parsing or hardware.
struct UBX_NAV_PVT_data_t {
  uint32_t iTOW = 0;
  int32_t lat = 0, lon = 0, height = 0;
  uint8_t numSV = 0, fixType = 0;
  struct { struct { bool gnssFixOK = false; } bits; } flags;
  struct { struct { bool invalidLlh = false; } bits; } flags3;
  struct { struct { bool validDate = false, validTime = false; } bits; } valid;
};
struct UBX_NAV_DOP_data_t { uint32_t iTOW; uint16_t hDOP; };
constexpr uint8_t COM_TYPE_UBX = 1;

struct SFE_UBLOX_GNSS {
  inline static bool present = true, configuration_ok = true;
  inline static unsigned reads = 0, config_calls = 0;
  inline static void (*pvt)(UBX_NAV_PVT_data_t*) = nullptr;
  inline static void (*dop)(UBX_NAV_DOP_data_t*) = nullptr;
  inline static std::vector<std::function<void()>> pending;
  bool begin(TwoWire&, uint8_t, uint16_t) { return present; }
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
  void checkUblox() { ++reads; }
  void checkCallbacks() {
    auto callbacks = std::move(pending);
    pending.clear();
    for (auto& callback : callbacks) callback();
  }
  uint32_t getUnixEpoch(uint16_t) { return 1700000000; }
};
