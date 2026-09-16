#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Arduino.h>
#include <Wire.h>
#include "accelerometer_config.h"
#include "activity_capture.h"

using namespace orun_tlp;
using Event = AccelerometerManager::Event;
using State = ActivityCapture::State;
using Start = ActivityCapture::StartResult;

void reset() {
  Wire = TwoWire{};
  fake_wire_timeout_flag = fake_wire_reset_required_flag = false;
  fake_accelerometer_present = true;
  fake_accelerometer_short_axis_read = false;
  fake_accelerometer_fail_write_register = -1;
  fake_accelerometer_write_count = fake_accelerometer_axis_read_count = 0;
  memset(fake_accelerometer_registers, 0, sizeof(fake_accelerometer_registers));
  fake_accelerometer_registers[0x0f] = 0x33;
  fake_accelerometer_registers[0x27] = 8;
  fake_accelerometer_registers[0x2d] = 0x3e; // 992 mg Z
}

uint32_t configure(AccelerometerManager& m, uint32_t now) {
  const unsigned first = fake_accelerometer_write_count;
  fake_accelerometer_registers[0x3e] = 127;
  for (unsigned i = 0; i < 11; ++i) {
    const auto count = fake_accelerometer_write_count;
    assert(m.poll(now++) == Event::kNone);
    assert(fake_accelerometer_write_count == count + 1);
  }
  assert(fake_accelerometer_write_regs[first] == 0x20);
  assert(fake_accelerometer_write_values[first] == 0);
  assert(fake_accelerometer_write_regs[first + 10] == 0x20);
  assert(fake_accelerometer_write_values[first + 10] == 0x27);
  assert(fake_accelerometer_registers[0x23] == 0x88);
  assert(fake_accelerometer_registers[0x3e] == 0);
  const unsigned reads = fake_accelerometer_axis_read_count;
  const uint32_t settled = now - 1 + 700;
  m.poll(settled - 1);
  assert(fake_accelerometer_axis_read_count == reads);
  m.poll(settled); // settle -> wait
  m.poll(settled + 1); // status
  m.poll(settled + 2); // retained discard
  assert(fake_accelerometer_axis_read_count == reads + 1);
  AccelerometerSample sample;
  assert(!m.takeRuntimeSample(&sample));
  return settled + 102;
}

uint32_t boot(AccelerometerManager& m) {
  m.begin(0);
  assert(!m.startRuntimeSession());
  m.poll(0);
  assert(!m.startRuntimeSession());
  uint32_t now = configure(m, 1);
  m.poll(now++);
  m.poll(now++);
  assert(!m.startRuntimeSession()); // sample read but not shut down
  assert(m.poll(now++) == Event::kPresent);
  assert(m.runtimeShutdownConfirmed());
  return now;
}

void admissionAndHandoff() {
  reset();
  AccelerometerManager m;
  ActivityCapture c(m);
  assert(c.start() == Start::kPending);
  uint32_t now = boot(m);
  assert(m.startRuntimeSession());
  assert(!m.startRuntimeSession());
  now = configure(m, now);
  m.poll(now++);
  m.poll(now++);
  const unsigned reads = fake_accelerometer_axis_read_count;
  fake_accelerometer_registers[0x2d] = 1;
  m.poll(now + 100);
  assert(fake_accelerometer_axis_read_count == reads);
  AccelerometerSample sample;
  assert(!m.takeRuntimeSample(nullptr));
  assert(m.takeRuntimeSample(&sample) && sample.z_mg == 992);
  assert(!m.takeRuntimeSample(&sample));
  m.stopRuntimeSession();
  assert(!m.runtimeShutdownConfirmed());
  fake_accelerometer_fail_write_register = 0x20;
  m.poll(now++);
  m.stopRuntimeSession(); // must not reset retry budget
  assert(m.runtimeSessionActive() && !m.runtimeShutdownConfirmed());
  fake_accelerometer_fail_write_register = -1;
  m.poll(now++);
  assert(m.runtimeShutdownConfirmed());
  assert(fake_accelerometer_registers[0x20] == 0);
  assert(m.startRuntimeSession());
  m.stopRuntimeSession(); // stop even before configuration
  m.poll(now);
  assert(m.runtimeShutdownConfirmed());

  reset();
  fake_accelerometer_present = false;
  AccelerometerManager absent;
  ActivityCapture unavailable(absent);
  absent.begin(0);
  absent.poll(0); absent.poll(250); absent.poll(500);
  assert(!absent.startRuntimeSession());
  assert(unavailable.start() == Start::kAbsent);
}

void capture(bool broken, bool stop_fault, uint32_t start_at) {
  reset();
  AccelerometerManager m;
  boot(m);
  ActivityCapture c(m);
  assert(c.state() == State::kIdle);
  assert(c.start() == Start::kStarted);
  assert(c.start() == Start::kBusy);
  uint32_t now = configure(m, start_at);
  const unsigned reads = fake_accelerometer_axis_read_count;
  for (unsigned i = 0; i < 50; ++i) {
    m.poll(now); // status
    m.poll(now); // axes, separate cooperative pass at same timestamp
    c.poll();
    assert(c.sampleCount() == i + 1);
    assert(c.result() == nullptr);
    assert(!c.assessment().usable);
    if (i < 49) assert(c.state() == State::kCapturing);
    now += broken && i == 10 ? 400 : 100;
  }
  assert(c.state() == State::kStopping);
  assert(fake_accelerometer_axis_read_count == reads + 50);
  assert(c.start() == Start::kBusy);
  fake_accelerometer_fail_write_register = 0x20;
  m.poll(now++); c.poll();
  assert(c.state() == State::kStopping && c.result() == nullptr);
  if (stop_fault) {
    m.poll(now++); c.poll();
    m.poll(now++); c.poll();
    assert(c.state() == State::kFault && c.result() == nullptr);
    assert(m.detected() && m.faulted());
    assert(c.start() == Start::kFault);
    const unsigned writes = fake_accelerometer_write_count;
    fake_accelerometer_fail_write_register = -1;
    m.poll(now + 59998);
    assert(fake_accelerometer_write_count == writes);
    m.poll(now + 59999);
    assert(fake_accelerometer_registers[0x20] == 0);
    assert(m.faulted() && !m.startRuntimeSession());
    return;
  }
  fake_accelerometer_fail_write_register = -1;
  m.poll(now++); c.poll();
  assert(c.state() == (broken ? State::kInvalid : State::kReady));
  assert(c.result() && c.result()->sample_count == 50);
  assert(c.assessment().usable == !broken);
  assert(c.result()->timing_discontinuities == (broken ? 1 : 0));
  const auto result = *c.result();
  for (unsigned i = 0; i < 10; ++i) { m.poll(now++); c.poll(); }
  assert(c.result()->duration_ms == result.duration_ms);
  assert(c.start() == Start::kStarted);
  assert(c.result() == nullptr && c.sampleCount() == 0);
  // A full second capture, not just a start acknowledgement.
  now = configure(m, now);
  for (unsigned i = 0; i < 50; ++i) {
    m.poll(now); m.poll(now); c.poll(); now += 100;
  }
  assert(c.state() == State::kStopping);
  m.poll(now); c.poll();
  assert(c.state() == State::kReady);
  assert(c.result()->duration_ms == 4900);
}

void runtimeFailures() {
  for (unsigned kind = 0; kind < 4; ++kind) {
    reset();
    AccelerometerManager m;
    uint32_t now = boot(m);
    ActivityCapture c(m);
    assert(c.start() == Start::kStarted);
    if (kind == 0) {
      fake_accelerometer_fail_write_register = 0x20;
      m.poll(now++);
    } else {
      now = configure(m, now);
      if (kind == 1) {
        fake_wire_timeout_flag = true;
        m.poll(now++);
      } else if (kind == 2) {
        m.poll(now++);
        fake_accelerometer_short_axis_read = true;
        m.poll(now++);
      } else {
        fake_accelerometer_registers[0x27] = 0;
        m.poll(now + 500); now += 501;
      }
    }
    c.poll();
    assert(m.detected() && m.faulted());
    assert(c.state() == State::kFault && !c.result());
    fake_accelerometer_fail_write_register = -1;
    assert(m.poll(now) == Event::kFault);
    assert(fake_accelerometer_registers[0x20] == 0);
    assert(!m.startRuntimeSession());
  }
  reset();
  AccelerometerManager m;
  uint32_t now = boot(m);
  assert(m.startRuntimeSession());
  now = configure(m, now);
  m.poll(now); m.poll(now);
  m.poll(now + 500); // abandoned unread sample is bounded
  assert(m.faulted() && m.detected());
  m.poll(now + 501);
  assert(fake_accelerometer_registers[0x20] == 0);
}

int main() {
  admissionAndHandoff();
  capture(false, false, 1000);
  capture(true, false, 1000);
  capture(false, true, 1000);
  capture(false, false, UINT32_MAX - 800);
  runtimeFailures();
  puts("M6B3 runtime session/capture checks: PASS");
}
