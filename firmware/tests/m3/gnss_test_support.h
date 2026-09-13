#pragma once
#include <assert.h>
#include <stdio.h>
#include "Arduino.h"
#include "gnss_config.h"
#include "gnss_manager.h"
#include "monotonic_time.h"
#include "sensor_power_manager.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;
using State = GnssManager::State;
using Fake = SFE_UBLOX_GNSS;
uint32_t test_now = 0;
uint32_t orun_tlp::monotonic::nowMs() { return test_now; }

UBX_NAV_PVT_data_t pvt(uint32_t tow) {
  UBX_NAV_PVT_data_t value{};
  value.iTOW = tow;
  value.flags.bits.gnssFixOK = true;
  value.fixType = 3;
  value.lat = 410000000;
  value.lon = 290000000;
  value.numSV = 8;
  value.year = 2023; value.month = 11; value.day = 14;
  value.hour = 22; value.min = 13; value.sec = 20;
  return value;
}

void emitPvt(GnssManager& manager, UBX_NAV_PVT_data_t value) {
  // A single parsed PVT is both the current cache and the callback snapshot.
  Fake::pending.push_back([value]() mutable {
    Fake::current_pvt = value;
    Fake::itow_fresh = true;
    Fake::pvt(&value);
  });
  manager.poll();
}
void emitDop(GnssManager& manager, uint32_t tow) {
  Fake::pending.push_back([tow]() { UBX_NAV_DOP_data_t value{tow, 123}; Fake::dop(&value); });
  manager.poll();
}
void prepare(GnssManager& manager) {
  for (unsigned i = 0; i < 7 && manager.state() == State::kStarting; ++i) manager.poll();
  assert(manager.state() == State::kAcquiring);
}
uint32_t boot(GnssManager& manager, uint32_t start = 0) {
  test_now = start;
  Fake::present = Fake::configuration_ok = true;
  Fake::pending.clear();
  Fake::callback_valid = false;
  Fake::current_pvt = {};
  Fake::callback_pvt = {};
  Fake::itow_fresh = false;
  Fake::time_of_week_cache_misses = 0;
  Fake::time_of_week_timeout_on_cache_miss = false;
  Fake::read_ok = true;
  fake_wire_timeout_flag = false;
  fake_wire_reset_required_flag = false;
  fake_scl_stuck_low = false;
  fake_sda_stuck_low = false;
  fake_sda_release_after_clocks = -1;
  fake_scl_clock_pulses = 0;
  Wire.status_ok = true;
  Wire.bytes_available = 0;
  Wire.register_pointer = 0;
  Wire.read_index = 0;
  Wire.begin_calls = Wire.end_calls = Wire.set_clock_calls = 0;
  Wire.last_clock_hz = 0;
  SensorPowerManager::begin();
  manager.begin();
  test_now += gnss_config::kPowerSettleMs;
  manager.poll();
  test_now += gnss_config::kPowerSettleMs;
  manager.poll();
  manager.poll();
  assert(manager.detected() && manager.state() == State::kStarting);
  assert(manager.diagnostics().acquisition_attempts == 1);
  const uint32_t anchor = test_now;
  prepare(manager);
  return anchor;
}
