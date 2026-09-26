// M7P7B temporary physical flash-concurrency probe: host coverage of the
// pure ConfigFlashProbe state machine against the REAL ConfigStore and a
// portable async fake FlashBackend (same pending pattern as
// test_m7p5_config_store.cpp). The fake also models the gate's config-client
// counters (accepted / success / error) so the probe's async-evidence rules
// are exercised. This proves the state machine only; the physical run
// (real SoftDevice + real BLE client) is a separate, unperformed gate.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "m7p7b_flash_probe.h"
#include "storage_config.h"

using namespace orun_tlp;
using Probe = ConfigFlashProbe;

namespace {
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize = kPageSize * storage_config::kFutureConfigRegionPages;

class ProbeIncarnationSource : public ConfigIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = 0xA1A2A3A4A5A6A7A8ULL;
    return true;
  }
};

class AsyncFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  uint32_t pending_steps = 1;   // every op takes >=1 extra poll (async)
  int fail_from_op = -1;        // 0-based op index from which resolves fail
  bool stall = false;           // op never completes
  uint32_t accepted = 0, successes = 0, errors = 0, ops = 0, raw_writes = 0;

  AsyncFlash() { bytes.fill(0xFF); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* data, size_t size) const override {
    if (offset > bytes.size() || size > bytes.size() - offset) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    assert(!in_flight_);
    ++raw_writes;
    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      if ((bytes[offset + i] & src[i]) != src[i]) return FlashOpResult::kFailed;
      bytes[offset + i] &= src[i];
    }
    return begin_op();
  }
  FlashOpResult erasePage(uint32_t page) override {
    assert(!in_flight_);
    ++raw_writes;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return begin_op();
  }
  FlashOpResult pollPending() override {
    assert(in_flight_);
    if (stall) return FlashOpResult::kPending;
    if (remaining_ > 0) { --remaining_; return FlashOpResult::kPending; }
    in_flight_ = false;
    if (fail_from_op >= 0 && static_cast<int>(ops) - 1 >= fail_from_op) {
      ++errors;
      return FlashOpResult::kFailed;
    }
    ++successes;
    return FlashOpResult::kDone;
  }

 private:
  FlashOpResult begin_op() {
    ++ops;
    if (pending_steps == 0) {
      // Model the production pre-SoftDevice ConfigStore baseline path:
      // NrfConfigFlash completes synchronously and no async completion event
      // or accepted-op counter is involved.
      ++successes;
      return FlashOpResult::kDone;
    }

    ++accepted;
    remaining_ = pending_steps;
    in_flight_ = true;
    return FlashOpResult::kPending;
  }
  bool in_flight_ = false;
  uint32_t remaining_ = 0;
};

struct Rig {
  AsyncFlash flash;
  ProbeIncarnationSource rng;
  ConfigStore store{flash, &rng};
  Probe probe;
  uint32_t now = 1000;
  uint8_t ble_connected = 1;
  uint32_t disconnect_events = 5;  // arbitrary non-zero start
  bool ble_ready = true;

  Rig() {
    // Production establishes the fresh v2 baseline before Bluefruit enables
    // SoftDevice, so model that one boot-time step synchronously. The probe
    // itself then exercises the async path exactly as before.
    flash.pending_steps = 0;
    assert(store.begin());
    flash.pending_steps = 1;

    bool ok = false;
    assert(store.requestSave(config_format::Config(600, 1234)));
    settle();
    assert(store.takeSaveResult(ok) && ok);
    flash.accepted = flash.successes = flash.errors = flash.ops =
        flash.raw_writes = 0;
  }
  void settle() { for (int i = 0; i < 200 && store.busy(); ++i) store.poll(); }

  Probe::Inputs inputs() const {
    Probe::Inputs in;
    in.now_ms = now;
    in.ble_connected = ble_connected;
    in.ble_disconnect_events = disconnect_events;
    in.async.async_accepted = flash.accepted;
    in.async.completions_success = flash.successes;
    in.async.completions_error = flash.errors;
    return in;
  }
  Probe::StartResult start() { return probe.start(store, ble_ready, inputs()); }
  // One cooperative loop tick: store.poll() then probe.poll(), like main.cpp.
  bool tick(uint32_t advance_ms = 1) {
    now += advance_ms;
    store.poll();
    return probe.poll(store, inputs());
  }
  // Runs ticks until the probe reports, bounded.
  bool run(uint32_t max_ticks = 500) {
    for (uint32_t i = 0; i < max_ticks; ++i)
      if (tick()) return true;
    return false;
  }
  bool original() const {
    return store.config().tracking_interval_seconds == 600 &&
           store.config().battery_capacity_mah == 1234;
  }
};
}  // namespace

int main() {
  // 1. Success: temp then exact restore, BLE stayed connected, async path used.
  {
    Rig r;
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.probe.temporaryConfig().battery_capacity_mah == (1234U ^ 1U));
    assert(r.probe.temporaryConfig().tracking_interval_seconds == 600);
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(rep.pass && rep.failure == Probe::Failure::kNone);
    assert(rep.temp_verified && rep.restore_verified && rep.ble_connected);
    assert(rep.ble_disconnects == 0 && rep.errors_delta == 0 && rep.timeouts_delta == 0);
    // Two saves x (erase + body + commit) through the async path.
    assert(rep.accepted_delta == 6 && rep.success_delta == 6);
    assert(r.original());
    assert(r.probe.state() == Probe::State::kDone);
    assert(!r.tick());  // final report is delivered exactly once
    r.probe.reset();
    assert(r.probe.state() == Probe::State::kIdle);
    assert(r.start() == Probe::StartResult::kStarted);  // re-armable
  }

  // 2. Fail-closed preconditions: config untouched, no flash activity.
  {
    Rig r;
    r.ble_ready = false;
    assert(r.start() == Probe::StartResult::kBleNotReady);
    r.ble_ready = true;
    r.ble_connected = 0;
    assert(r.start() == Probe::StartResult::kBleClientCount);
    r.ble_connected = 2;
    assert(r.start() == Probe::StartResult::kBleClientCount);
    r.ble_connected = 1;
    assert(r.flash.raw_writes == 0 && r.original());
    assert(r.probe.state() == Probe::State::kIdle);
    // Store busy -> refuse.
    assert(r.store.requestSave(config_format::Config(700, 1)));
    assert(r.start() == Probe::StartResult::kConfigNotReady);
    r.settle();
    bool ok;
    assert(r.store.takeSaveResult(ok) && ok);
    // Not idle -> refuse.
    r.store.requestSave(config_format::Config(600, 1234));
    r.settle();
    assert(r.store.takeSaveResult(ok));
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.start() == Probe::StartResult::kNotIdle);
    // ConfigStore not ready at all.
    AsyncFlash unready_flash;
    ConfigStore unready(unready_flash);
    Probe p2;
    assert(p2.start(unready, true, Probe::Inputs()) == Probe::StartResult::kConfigNotReady);
  }

  // 3. A stale unread ConfigStore result cannot be mistaken for the probe's.
  {
    Rig r;
    assert(r.store.requestSave(config_format::Config(600, 99)));
    r.settle();  // result left unread on purpose
    assert(r.store.requestSave(config_format::Config(600, 1234)) == false);  // blocked
    // (config is now 600/99: that is the "original" the probe must restore)
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.probe.staleResultDrained());
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(rep.pass);
    assert(rep.original.battery_capacity_mah == 99);
    assert(r.store.config().battery_capacity_mah == 99);
  }

  // 4. BLE disconnect mid-test: restoration still completes, result FAIL(ble).
  {
    Rig r;
    assert(r.start() == Probe::StartResult::kStarted);
    r.tick();
    r.disconnect_events += 1;  // disconnect + reconnect: connected==1 again
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(!rep.pass && rep.failure == Probe::Failure::kBleLost);
    assert(rep.temp_verified && rep.restore_verified && !rep.restore_failure);
    assert(rep.ble_disconnects == 1 && rep.ble_connected);
    assert(r.original());  // restored despite the disconnect
  }
  {
    Rig r;  // still disconnected at the end
    assert(r.start() == Probe::StartResult::kStarted);
    r.tick();
    r.ble_connected = 0;
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(!rep.pass && rep.failure == Probe::Failure::kBleLost && !rep.ble_connected);
    assert(r.original());
  }

  // 5. Temp save fails: last-good kept, NO cleanup writes, FAIL temp-save.
  {
    Rig r;
    r.flash.fail_from_op = 0;  // the erase fails
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(!rep.pass && rep.failure == Probe::Failure::kTempSave && !rep.restore_failure);
    assert(r.original());
    assert(r.flash.ops == 1);  // nothing further was attempted
  }

  // 6. Restore fails: unmistakable restore failure, never PASS, values kept.
  {
    Rig r;
    r.flash.fail_from_op = 3;  // temp save = ops 0..2 succeed; restore fails
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.run());
    const auto& rep = r.probe.report();
    assert(!rep.pass && rep.failure == Probe::Failure::kRestoreSave);
    assert(rep.restore_failure && rep.temp_verified && !rep.restore_verified);
    assert(rep.original.battery_capacity_mah == 1234);
    assert(rep.current.battery_capacity_mah == (1234U ^ 1U));  // temp still active
    assert(rep.errors_delta >= 1);
  }

  // 7. Async evidence: an error/timeout/late delta or no accepted ops fails PASS.
  {
    Rig r;
    assert(r.start() == Probe::StartResult::kStarted);
    // Synthetic: gate reports a timeout during the run.
    bool done = false;
    for (int i = 0; i < 500 && !done; ++i) {
      r.now += 1;
      r.store.poll();
      Probe::Inputs in = r.inputs();
      in.async.timeouts = 1;
      done = r.probe.poll(r.store, in);
    }
    assert(done && !r.probe.report().pass);
    assert(r.probe.report().failure == Probe::Failure::kAsyncEvidence);
    assert(r.original());
  }
  {
    Rig r;  // ConfigStore changed but the gate counters never moved
    assert(r.start() == Probe::StartResult::kStarted);
    bool done = false;
    for (int i = 0; i < 500 && !done; ++i) {
      r.now += 1;
      r.store.poll();
      Probe::Inputs in = r.inputs();
      in.async = Probe::AsyncCounters();
      done = r.probe.poll(r.store, in);
    }
    assert(done && r.probe.report().failure == Probe::Failure::kAsyncEvidence);
  }

  // 8. Overall timeout: a stalled flash op ends in FAIL timeout, never PASS.
  {
    Rig r;
    r.flash.stall = true;
    assert(r.start() == Probe::StartResult::kStarted);
    assert(!r.tick(Probe::kOverallTimeoutMs - 10));
    assert(r.tick(20));
    const auto& rep = r.probe.report();
    assert(!rep.pass && rep.failure == Probe::Failure::kTimeout);
  }

  // 9. Loop time rollover does not break the deadline.
  {
    Rig r;
    r.now = UINT32_MAX - 100;
    assert(r.start() == Probe::StartResult::kStarted);
    assert(r.run());
    assert(r.probe.report().pass);
  }

  printf("M7P7B flash probe state machine checks: PASS\n");
  return 0;
}
