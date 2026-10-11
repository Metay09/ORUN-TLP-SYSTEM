#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Arduino.h>
#include <Wire.h>
#include "accelerometer_config.h"
#include "activity_capture.h"
#include "activity_auto_sampler.h"
#include "activity_period_evidence.h"

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
  fake_accelerometer_max_request = fake_accelerometer_fail_reads = 0;
  fakeFifoClear();
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

// ---- Continuous FIFO stream (owner decision 2026-10-12) ----------------

constexpr int16_t kStillZ = 992;    // mg; multiples of 4 (10-bit, 4 mg/digit)
constexpr int16_t kMovingLowZ = 768;

// Starts the stream on a booted manager: twelve configuration writes, one
// per pass, then Stream mode after the settle. Returns that time.
uint32_t startStream(AccelerometerManager& m, uint32_t now) {
  assert(m.startStream());
  assert(!m.startStream());
  const unsigned first = fake_accelerometer_write_count;
  fake_accelerometer_registers[0x2E] = 0x80;  // retained across an MCU reset
  for (unsigned i = 0; i < 12; ++i) {
    const auto count = fake_accelerometer_write_count;
    assert(m.poll(now++) == Event::kNone);
    assert(fake_accelerometer_write_count == count + 1);
  }
  assert(fake_accelerometer_write_regs[first] == 0x20 &&
         fake_accelerometer_write_values[first] == 0);
  assert(fake_accelerometer_write_regs[first + 11] == 0x20 &&
         fake_accelerometer_write_values[first + 11] == 0x27);
  assert(fake_accelerometer_registers[0x24] == 0x40);  // FIFO enabled
  assert(fake_accelerometer_registers[0x23] == 0x80);  // BDU, normal, 2 g
  assert(fake_accelerometer_registers[0x2E] == 0x00);  // Bypass: empty
  assert(fake_accelerometer_registers[0x3E] == 0);
  const uint32_t settled = now - 1 + accelerometer_config::kStreamSettleMs;
  m.poll(settled - 1);
  assert(!fakeFifoStreaming());
  fakeFifoPush(0, 0, 4000);  // a turn-on sample is not stored in Bypass
  m.poll(settled);
  assert(fakeFifoStreaming() && fake_fifo_count == 0);
  return settled;
}

// The sensor produces one sample per 100 ms; the loop polls the manager and
// then the sampler several times per tick (one bus operation per pass).
uint32_t runStream(AccelerometerManager& m, ActivityAutoSampler* s,
                   uint32_t now, uint32_t duration_ms, bool moving,
                   unsigned* tick) {
  for (uint32_t t = 0; t < duration_ms; t += 100) {
    now += 100;
    const int16_t z = moving && (*tick % 2u) ? kMovingLowZ : kStillZ;
    fakeFifoPush(0, 0, z);
    ++*tick;
    for (unsigned pass = 0; pass < 4; ++pass) {
      m.poll(now);
      if (s != nullptr) s->poll(m, now);
    }
  }
  return now;
}

void streamDrainsInOrder() {
  reset();
  AccelerometerManager m;
  uint32_t now = boot(m);
  const uint32_t started = startStream(m, now);
  ActivityCapture diagnostic(m);
  assert(diagnostic.start() == Start::kBusy);  // the stream owns the sensor
  assert(!m.startRuntimeSession());

  unsigned tick = 0;
  now = runStream(m, nullptr, started, 1900, false, &tick);
  AccelerometerStreamSample sample;
  assert(!m.takeStreamSample(&sample));  // nothing before the first drain
  now = runStream(m, nullptr, now, 100, false, &tick);  // drain at +2000
  for (unsigned i = 0; i < 20; ++i) {
    assert(m.takeStreamSample(&sample));
    assert(sample.continuity_break == (i == 0));
    assert(sample.sample.captured_at_ms == started + 100 + 100 * i);
    assert(sample.sample.z_mg == kStillZ && sample.sample.x_mg == 0);
  }
  assert(!m.takeStreamSample(&sample));
  assert(fake_accelerometer_max_request <= 60);
  assert(m.diagnostics().stream_drains == 1);
  assert(m.diagnostics().stream_samples == 20);

  // A late drain: 40 samples arrive, the FIFO keeps the newest 32 and flags
  // the overrun. FSS reads 31, so one sample waits for the next drain.
  for (unsigned i = 0; i < 40; ++i) fakeFifoPush(0, 0, kStillZ);
  now += 4000;
  for (unsigned pass = 0; pass < 5; ++pass) m.poll(now);
  assert(m.diagnostics().stream_overruns == 1);
  for (unsigned i = 0; i < 31; ++i) {
    assert(m.takeStreamSample(&sample));
    assert(sample.continuity_break == (i == 0));
  }
  assert(!m.takeStreamSample(&sample) && fake_fifo_count == 1);
  now = runStream(m, nullptr, now, 2000, false, &tick);
  for (unsigned i = 0; i < 21; ++i) {
    assert(m.takeStreamSample(&sample) && !sample.continuity_break);
  }
  assert(m.diagnostics().stream_timing_errors == 0);

  // One failed read only breaks continuity; the next drain is kept.
  fake_accelerometer_fail_reads = 1;
  now = runStream(m, nullptr, now, 2000, false, &tick);
  assert(m.diagnostics().stream_read_errors == 1 && !m.faulted());
  assert(!m.takeStreamSample(&sample));
  now = runStream(m, nullptr, now, 2000, false, &tick);
  assert(m.takeStreamSample(&sample) && sample.continuity_break);

  // Stop: bounded power-down, then the one-shot path is available again.
  m.stopStream();
  m.stopStream();  // repeated stop keeps the shutdown budget
  m.poll(now);
  assert(fake_accelerometer_registers[0x20] == 0);
  assert(!m.streamActive() && m.runtimeShutdownConfirmed());
  assert(!m.takeStreamSample(&sample));
  assert(m.startRuntimeSession());
  m.stopRuntimeSession();
  m.poll(now + 1);
  assert(m.runtimeShutdownConfirmed());
}

void streamTimingAndReadFaults() {
  // A sensor not sampling at 10 Hz: samples are read out but not kept, and
  // the third implausible drain in a row is a sensor fault.
  reset();
  AccelerometerManager m;
  uint32_t now = startStream(m, boot(m));
  AccelerometerStreamSample sample;
  for (unsigned drain = 1; drain <= 3; ++drain) {
    for (unsigned i = 0; i < 5; ++i) fakeFifoPush(0, 0, kStillZ);
    now += 2000;
    for (unsigned pass = 0; pass < 3; ++pass) m.poll(now);
    assert(m.diagnostics().stream_timing_errors == drain);
    assert(!m.takeStreamSample(&sample));
    assert(m.faulted() == (drain == 3));
  }
  m.poll(now);
  assert(fake_accelerometer_registers[0x20] == 0 && !m.streamActive());
  assert(!m.startStream());

  // Too many samples for the elapsed time is implausible too.
  reset();
  AccelerometerManager fast;
  now = startStream(fast, boot(fast));
  for (unsigned i = 0; i < 30; ++i) fakeFifoPush(0, 0, kStillZ);
  now += 2000;
  for (unsigned pass = 0; pass < 4; ++pass) fast.poll(now);
  assert(fast.diagnostics().stream_timing_errors == 1 && !fast.faulted());
  assert(!fast.takeStreamSample(&sample) && fake_fifo_count == 0);

  // Three failed drains in a row are a sensor fault.
  reset();
  AccelerometerManager failing;
  now = startStream(failing, boot(failing));
  fake_accelerometer_fail_reads = 3;
  for (unsigned drain = 1; drain <= 3; ++drain) {
    now += 2000;
    failing.poll(now);
  }
  assert(failing.diagnostics().stream_read_errors == 3 && failing.faulted());
}

void autoReportOwnedContinuous() {
  reset();
  AccelerometerManager m;
  uint32_t now = boot(m);
  ActivityAutoSampler sampler;
  sampler.setReportOwned(true);
  const uint32_t opened = now;
  sampler.setEnabled(true, opened);
  sampler.setEnabled(true, opened + 1);  // ON is idempotent
  assert(!sampler.closePeriod(opened + 999));  // shorter than one second

  // One minute moving, one still, one moving: a three-minute report period.
  unsigned tick = 0;
  now = runStream(m, &sampler, now, 60000, true, &tick);
  assert(sampler.streaming() && m.streamActive());
  now = runStream(m, &sampler, now, 60000, false, &tick);
  now = runStream(m, &sampler, now, 60000, true, &tick);
  // No automatic hourly cut while report-owned, also after an hour.
  assert(!sampler.poll(m, opened + activity_auto_config::kSummaryPeriodMs));
  const uint32_t closed = now;
  assert(sampler.closePeriod(closed));
  const ActivityHourSummary summary = sampler.latestSummary();
  assert(summary.started_at_ms == opened && summary.finished_at_ms == closed);
  assert(summary.duration_seconds == 180);
  assert(summary.invalid_windows == 0 && summary.fault_windows == 0);
  // Back-to-back 5 s windows: the whole period except the stream start and
  // the window still filling at the close (at most 5 s each).
  assert(summary.measured_coverage_ms ==
         summary.usable_windows * activity_auto_config::kWindowCoverageMs);
  // Stream mode starts 0.6 s in; drains every 2 s leave the last samples
  // (178.7-180 s) in a window still filling at the close: 35 windows, 175 s.
  assert(summary.usable_windows == 35 && summary.measured_coverage_ms == 175000);
  assert(summary.boundary_discarded_windows == 1);
  // Windows over the still minute are inactive; the two windows straddling a
  // change still carry movement (12 + 12 active). Exactly two transitions.
  assert(summary.active_windows == 24);
  assert(summary.activity_transitions == 2);
  assert(summary.measured_active_ms ==
         summary.active_windows * activity_auto_config::kWindowCoverageMs);
  assert(sampler.lastMeanMagnitudeSquaredMg2() > 600000);

  ActivityPeriodEvidence evidence{};
  assert(linkActivityPeriodEvidence(summary, opened, closed, evidence) ==
         ActivityPeriodLinkResult::kLinkedClassified);
  assert(evidence.coverage_seconds == summary.measured_coverage_ms / 1000U);
  assert(evidence.unknown_seconds == 180 - evidence.coverage_seconds);
  assert(evidence.active_seconds <= evidence.coverage_seconds);
  assert(evidence.transition_count == 2);
  // A different report period is not this summary.
  assert(linkActivityPeriodEvidence(summary, opened + 1U, closed, evidence) ==
         ActivityPeriodLinkResult::kPeriodMismatch);
  ActivityHourSummary corrupt = summary;
  corrupt.measured_active_ms = summary.measured_coverage_ms + 1;
  assert(linkActivityPeriodEvidence(corrupt, opened, closed, evidence) ==
         ActivityPeriodLinkResult::kInvalidPeriod);

  // Close between drains: samples still in the FIFO at the close belong to
  // the closed period and never enter the next one.
  now = runStream(m, &sampler, now, 1000, false, &tick);
  const uint32_t mid = now;
  assert(sampler.closePeriod(mid));
  now = runStream(m, &sampler, now, 20000, false, &tick);
  assert(sampler.closePeriod(now));
  const ActivityHourSummary next = sampler.latestSummary();
  assert(next.started_at_ms == mid && next.duration_seconds == 20);
  assert(next.measured_coverage_ms <= 20000);
  assert(next.measured_coverage_ms >= 15000);
  assert(next.active_windows == 0 && next.activity_transitions == 0);

  // A stall longer than the FIFO: the window it breaks is dropped and the
  // lost time stays unmeasured.
  now = runStream(m, &sampler, now, 10000, false, &tick);
  for (unsigned i = 0; i < 40; ++i) fakeFifoPush(0, 0, kStillZ);
  tick += 40;
  now += 4000;
  now = runStream(m, &sampler, now, 10000, false, &tick);
  assert(sampler.closePeriod(now));
  const ActivityHourSummary stalled = sampler.latestSummary();
  assert(stalled.duration_seconds == 24);
  assert(stalled.invalid_windows == 1);
  assert(stalled.measured_coverage_ms <= 20000);
  assert(m.diagnostics().stream_overruns == 1);

  // OFF stops the stream through the bounded power-down.
  sampler.setEnabled(false, now);
  sampler.poll(m, now);
  assert(!sampler.streaming());
  m.poll(now);
  assert(!m.streamActive() && fake_accelerometer_registers[0x20] == 0);
  assert(!sampler.closePeriod(now + 5000));  // disabled
}

void autoHourlySummary() {
  reset();
  AccelerometerManager m;
  uint32_t now = boot(m);
  ActivityAutoSampler sampler;
  const uint32_t enabled_at = now;
  sampler.setEnabled(true, enabled_at);
  unsigned tick = 0;
  now = runStream(m, &sampler, now, activity_auto_config::kSummaryPeriodMs - 100,
                  false, &tick);
  assert(!sampler.summaryReady());
  // The automatic publication happens on the hour.
  now += 100;
  fakeFifoPush(0, 0, kStillZ);
  m.poll(now);
  assert(sampler.poll(m, now));
  const ActivityHourSummary summary = sampler.latestSummary();
  assert(summary.started_at_ms == enabled_at && summary.duration_seconds == 3600);
  assert(summary.measured_coverage_ms >= 3590000);
  assert(summary.active_windows == 0 && summary.activity_transitions == 0);
  assert(summary.mean_axis_variance_sum_mg2 == 0);
  ActivityPeriodEvidence evidence{};
  assert(linkActivityPeriodEvidence(summary, enabled_at, now, evidence) ==
         ActivityPeriodLinkResult::kLinkedClassified);
  assert(evidence.coverage_seconds + evidence.unknown_seconds == 3600);
  assert(evidence.active_seconds == 0);
  // A 3-minute tracker record is not the 1-hour activity summary.
  assert(linkActivityPeriodEvidence(summary, enabled_at, enabled_at + 180000U,
                                    evidence) ==
         ActivityPeriodLinkResult::kPeriodMismatch);
  assert(sampler.publishedSummaries() == 1);
}

void autoUnavailableFaultAndRollover() {
  // Absent sensor across the 32-bit wrap: bounded retries, honest zero.
  reset();
  fake_accelerometer_present = false;
  AccelerometerManager absent;
  absent.begin(0);
  absent.poll(0); absent.poll(250); absent.poll(500);
  assert(absent.detectionComplete() && !absent.detected());
  ActivityAutoSampler sampler;
  const uint32_t started = UINT32_MAX - 1000U;
  sampler.setEnabled(true, started);
  assert(!sampler.poll(absent, started));
  assert(sampler.unavailableAttempts() == 1);
  assert(!sampler.poll(absent, started + 100U));
  assert(sampler.unavailableAttempts() == 1);
  assert(!sampler.poll(absent,
                       started + activity_auto_config::kUnavailableRetryMs));
  assert(sampler.unavailableAttempts() == 2);
  assert(sampler.poll(absent, started + activity_auto_config::kSummaryPeriodMs));
  assert(sampler.latestSummary().usable_windows == 0);
  ActivityPeriodEvidence evidence{};
  assert(linkActivityPeriodEvidence(sampler.latestSummary(), started,
             started + activity_auto_config::kSummaryPeriodMs, evidence) ==
         ActivityPeriodLinkResult::kNoUsableEvidence);
  assert(evidence.coverage_seconds == 0 && evidence.unknown_seconds == 3600);

  // Pending detection waits; a fault mid-stream ends it and counts once.
  reset();
  AccelerometerManager m;
  ActivityAutoSampler streaming;
  streaming.setReportOwned(true);
  streaming.setEnabled(true, 0);
  m.begin(0);
  assert(!streaming.poll(m, 0) && !streaming.streaming());
  assert(streaming.unavailableAttempts() == 0);
  uint32_t now = boot(m);
  unsigned tick = 0;
  now = runStream(m, &streaming, now + 1000, 12000, false, &tick);
  assert(streaming.streaming() && streaming.usableWindows() >= 1);
  fake_accelerometer_fail_reads = 3;
  now = runStream(m, &streaming, now, 6000, false, &tick);
  assert(m.faulted() && !streaming.streaming());
  assert(streaming.faultWindows() == 1);
  now = runStream(m, &streaming, now,
                  activity_auto_config::kUnavailableRetryMs, false, &tick);
  assert(streaming.unavailableAttempts() == 1 && streaming.faultWindows() == 1);
}

// ---- Head position (published grazing rule, 2026-10-12) -----------------

void headPoseClassifier() {
  using H = HeadPose;
  const NeckAxis x = NeckAxis::kPlusX;
  assert(classifyHeadPose(0, 0, 1000, x, H::kDown) == H::kUp);      // level
  assert(classifyHeadPose(-500, 0, 866, x, H::kUp) == H::kDown);    // -30 deg
  assert(classifyHeadPose(-423, 0, 906, x, H::kUp) == H::kDown);    // -25 deg
  assert(classifyHeadPose(-87, 0, 996, x, H::kDown) == H::kUp);     // -5 deg
  assert(classifyHeadPose(500, 0, 866, x, H::kDown) == H::kUp);     // +30 deg
  // -15 deg is inside the -20..-10 band: the previous position stays.
  assert(classifyHeadPose(-259, 0, 966, x, H::kUp) == H::kUp);
  assert(classifyHeadPose(-259, 0, 966, x, H::kDown) == H::kDown);
  // Band edges: -21 deg is down, -9 deg is up, from either side.
  assert(classifyHeadPose(-358, 0, 934, x, H::kUp) == H::kDown);
  assert(classifyHeadPose(-156, 0, 988, x, H::kDown) == H::kUp);
  // Rotation of the collar around the neck keeps the forward component.
  assert(classifyHeadPose(-500, 600, 624, x, H::kUp) == H::kDown);
  // Every axis and sign.
  assert(classifyHeadPose(500, 0, 866, NeckAxis::kMinusX, H::kUp) == H::kDown);
  assert(classifyHeadPose(0, -500, 866, NeckAxis::kPlusY, H::kUp) == H::kDown);
  assert(classifyHeadPose(0, 500, 866, NeckAxis::kMinusY, H::kUp) == H::kDown);
  assert(classifyHeadPose(0, 866, -500, NeckAxis::kPlusZ, H::kUp) == H::kDown);
  assert(classifyHeadPose(0, 866, 500, NeckAxis::kMinusZ, H::kUp) == H::kDown);
  // Unset axis or an unusable gravity reading keep the previous position.
  assert(classifyHeadPose(-500, 0, 866, NeckAxis::kUnset, H::kUp) == H::kUp);
  assert(classifyHeadPose(-300, 0, 100, x, H::kUp) == H::kUp);
  assert(classifyHeadPose(-1500, 0, 900, x, H::kUp) == H::kUp);
  assert(neckForwardComponent(1, 2, 3, NeckAxis::kMinusZ) == -3);
  assert(strcmp(neckAxisName(NeckAxis::kMinusY), "-Y") == 0);
  assert(strcmp(neckAxisName(NeckAxis::kUnset), "OFF") == 0);
}

// Like runStream, with the neck tilted 34 degrees down (+X forward) when
// head_down, still or moving (Z alternating by 224 mg).
uint32_t runPose(AccelerometerManager& m, ActivityAutoSampler& s, uint32_t now,
                 uint32_t duration_ms, bool head_down, bool moving,
                 unsigned* tick) {
  for (uint32_t t = 0; t < duration_ms; t += 100) {
    now += 100;
    const int16_t base_z = head_down ? 860 : kStillZ;
    const int16_t z =
        moving && (*tick % 2u) ? static_cast<int16_t>(base_z - 224) : base_z;
    fakeFifoPush(head_down ? -500 : 0, 0, z);
    ++*tick;
    for (unsigned pass = 0; pass < 4; ++pass) {
      m.poll(now);
      s.poll(m, now);
    }
  }
  return now;
}

void autoHeadDownShare() {
  reset();
  AccelerometerManager m;
  uint32_t now = boot(m);
  ActivityAutoSampler sampler;
  sampler.setReportOwned(true);
  assert(sampler.neckForwardAxis() == activity_classifier::kDefaultNeckForwardAxis);
  sampler.setNeckForwardAxis(NeckAxis::kPlusX);
  const uint32_t opened = now;
  sampler.setEnabled(true, opened);
  unsigned tick = 0;
  // Grazing minute (head down, moving), resting minute (head up, still),
  // walking minute (head up, moving).
  now = runPose(m, sampler, now, 60000, true, true, &tick);
  assert(sampler.headPose() == HeadPose::kDown);
  assert(sampler.lastMeanXMg() == -500);
  now = runPose(m, sampler, now, 60000, false, false, &tick);
  assert(sampler.headPose() == HeadPose::kUp);
  now = runPose(m, sampler, now, 60000, false, true, &tick);
  assert(sampler.closePeriod(now));
  const ActivityHourSummary summary = sampler.latestSummary();
  // As in autoReportOwnedContinuous: 35 windows, 24 active. The 12 windows of
  // the first minute (the last one straddling into the still minute, still
  // tilted 30 degrees on average) are head down.
  assert(summary.usable_windows == 35 && summary.active_windows == 24);
  assert(summary.head_pose_classified);
  assert(summary.measured_head_down_active_ms == 12 * 5000U);
  ActivityPeriodEvidence evidence{};
  assert(linkActivityPeriodEvidence(summary, opened, now, evidence) ==
         ActivityPeriodLinkResult::kLinkedClassified);
  assert(evidence.head_pose_valid && evidence.head_down_share == 100);  // 50 %
  // Corrupt head-down time never passes.
  ActivityHourSummary corrupt = summary;
  corrupt.measured_head_down_active_ms = summary.measured_active_ms + 1;
  assert(linkActivityPeriodEvidence(corrupt, opened, now, evidence) ==
         ActivityPeriodLinkResult::kInvalidPeriod);

  // Axis unknown for part of a period: no head position for that period.
  const uint32_t second = now;
  sampler.setNeckForwardAxis(NeckAxis::kUnset);
  now = runPose(m, sampler, now, 20000, true, true, &tick);
  sampler.setNeckForwardAxis(NeckAxis::kPlusX);
  now = runPose(m, sampler, now, 20000, true, true, &tick);
  assert(sampler.closePeriod(now));
  assert(!sampler.latestSummary().head_pose_classified);
  assert(sampler.latestSummary().measured_head_down_active_ms == 0);
  assert(linkActivityPeriodEvidence(sampler.latestSummary(), second, now,
                                    evidence) ==
         ActivityPeriodLinkResult::kLinkedClassified);
  assert(!evidence.head_pose_valid && evidence.head_down_share == 0);

  // A resting period with a known axis: valid head position, share zero.
  const uint32_t third = now;
  now = runPose(m, sampler, now, 20000, true, false, &tick);
  assert(sampler.closePeriod(now));
  assert(sampler.latestSummary().head_pose_classified);
  assert(sampler.latestSummary().active_windows == 0);
  assert(linkActivityPeriodEvidence(sampler.latestSummary(), third, now,
                                    evidence) ==
         ActivityPeriodLinkResult::kLinkedClassified);
  assert(evidence.head_pose_valid && evidence.head_down_share == 0);
}

int main() {
  admissionAndHandoff();
  capture(false, false, 1000);
  capture(true, false, 1000);
  capture(false, true, 1000);
  capture(false, false, UINT32_MAX - 800);
  runtimeFailures();
  streamDrainsInOrder();
  streamTimingAndReadFaults();
  autoReportOwnedContinuous();
  autoHourlySummary();
  autoUnavailableFaultAndRollover();
  headPoseClassifier();
  autoHeadDownShare();
  puts("M6B3 runtime session/capture and continuous stream checks: PASS");
}
