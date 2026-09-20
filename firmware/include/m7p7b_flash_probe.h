#pragma once
// M7P7B TEMPORARY, TEST-ONLY physical probe: one real ConfigStore mutation
// (through FlashMutationGate and the SoftDevice async flash path) while a BLE
// client is connected. Only the dedicated `rak4630_m7p7b_flash_probe`
// PlatformIO env (-DORUN_M7P7B_FLASH_PROBE=1) instantiates or dispatches
// this; the production `rak4630` image neither contains nor recognizes it.
// Header-only and pure (no Arduino/Bluefruit/gate dependency) so the state
// machine is host-tested; main.cpp owns every hardware input and all output.
//
// It uses only the public ConfigStore API (requestSave / takeSaveResult /
// config). It never touches flash, the gate, History, Security or bonds
// directly, owns no mutex/queue/event consumer, never blocks, never
// allocates, and is only ever stepped from the cooperative loop.
//
// State machine:
//   kIdle --start()--> kTempSaving --(temp save OK, config()==temp)-->
//   kRestoring --(restore OK, config()==original)--> evaluate --> kDone.
// Once the temporary config has committed, restoration of the exact original
// is mandatory and is attempted regardless of BLE state. A temp save that
// never succeeds is never "cleaned up" with extra writes: ConfigStore's own
// last-good behavior applies.
#include <stdint.h>

#include "config_format.h"
#include "config_store.h"
#include "monotonic_time.h"

namespace orun_tlp {

class ConfigFlashProbe {
 public:
  // Snapshot of the gate's config-client diagnostics (main.cpp fills it from
  // FlashMutationGate::configDiagnostics()).
  struct AsyncCounters {
    uint32_t async_accepted = 0;
    uint32_t completions_success = 0;
    uint32_t completions_error = 0;
    uint32_t timeouts = 0;
    uint32_t late_completions = 0;
  };
  // Everything the loop samples for one step.
  struct Inputs {
    uint32_t now_ms = 0;
    uint8_t ble_connected = 0;         // Bluefruit.Periph.connected()
    uint32_t ble_disconnect_events = 0;  // ble_disconnect_events snapshot
    AsyncCounters async;
  };

  enum class State : uint8_t { kIdle, kTempSaving, kRestoring, kDone };
  enum class StartResult : uint8_t {
    kStarted,
    kNotIdle,
    kConfigNotReady,
    kBleNotReady,
    kBleClientCount,  // connected() != 1
    kTempRejected,    // ConfigStore refused the temp request; config untouched
  };
  enum class Failure : uint8_t {
    kNone,
    kTempSave,        // temp save reported failure (last-good kept, no cleanup)
    kTempVerify,      // temp save OK but config() != temp
    kRestoreRequest,  // ConfigStore refused the restore request
    kRestoreSave,     // restore save reported failure
    kRestoreVerify,   // restore save OK but config() != original
    kTimeout,         // overall deadline expired
    kBleLost,         // BLE not connected==1 or a disconnect occurred
    kAsyncEvidence,   // async path not (cleanly) exercised
  };
  struct Report {
    bool pass = false;
    Failure failure = Failure::kNone;
    // True when the failure concerns the restore of the original config.
    bool restore_failure = false;
    bool temp_verified = false;
    bool restore_verified = false;
    bool ble_connected = false;
    uint32_t ble_disconnects = 0;
    uint32_t accepted_delta = 0;
    uint32_t success_delta = 0;
    uint32_t errors_delta = 0;
    uint32_t timeouts_delta = 0;
    uint32_t late_delta = 0;
    config_format::Config original;
    config_format::Config temporary;
    config_format::Config current;  // config() when the report was made
  };

  // Overall bound for the whole sequence. ConfigStore's worst case is 6 gate
  // operations, each bounded by the gate's own 4 s operation timeout.
  static constexpr uint32_t kOverallTimeoutMs = 60UL * 1000UL;

  State state() const { return state_; }
  const Report& report() const { return report_; }
  const config_format::Config& temporaryConfig() const { return report_.temporary; }

  StartResult start(ConfigStore& store, bool ble_ready, const Inputs& in) {
    if (state_ != State::kIdle) return StartResult::kNotIdle;
    if (!store.ready() || store.busy()) return StartResult::kConfigNotReady;
    if (!ble_ready) return StartResult::kBleNotReady;
    if (in.ble_connected != 1) return StartResult::kBleClientCount;
    // No caller consumes ConfigStore results in this firmware; drop anything
    // stale so it can never be mistaken for this run's result. (Does not
    // touch the config or flash.)
    bool stale_success = false;
    stale_drained_ = store.takeSaveResult(stale_success);

    report_ = Report();
    report_.original = store.config();
    // battery_capacity_mah is not consumed by any runtime behavior (power,
    // GNSS, radio); toggling bit 0 always differs and is fully reversible.
    report_.temporary = config_format::Config(
        report_.original.tracking_interval_seconds,
        report_.original.battery_capacity_mah ^ 1U);
    start_events_ = in.ble_disconnect_events;
    start_async_ = in.async;
    started_ms_ = in.now_ms;
    if (!store.requestSave(report_.temporary)) {
      report_.current = store.config();
      return StartResult::kTempRejected;  // state stays kIdle, config untouched
    }
    state_ = State::kTempSaving;
    return StartResult::kStarted;
  }

  // Call once per loop tick AFTER config_store.poll(). Returns true exactly
  // once, on the tick the final report becomes available.
  bool poll(ConfigStore& store, const Inputs& in) {
    if (state_ == State::kIdle || state_ == State::kDone) return false;
    if (monotonic::reached(in.now_ms, started_ms_ + kOverallTimeoutMs)) {
      return finish(store, in, Failure::kTimeout);
    }
    bool success = false;
    if (state_ == State::kTempSaving) {
      if (!store.takeSaveResult(success)) return false;
      if (!success) return finish(store, in, Failure::kTempSave);
      report_.temp_verified = sameConfig(store.config(), report_.temporary);
      // The temp config is committed regardless: restoring is mandatory.
      if (!store.requestSave(report_.original)) {
        return finish(store, in, Failure::kRestoreRequest);
      }
      state_ = State::kRestoring;
      return false;
    }
    // kRestoring
    if (!store.takeSaveResult(success)) return false;
    if (!success) return finish(store, in, Failure::kRestoreSave);
    report_.restore_verified = sameConfig(store.config(), report_.original);
    if (!report_.restore_verified) return finish(store, in, Failure::kRestoreVerify);
    if (!report_.temp_verified) return finish(store, in, Failure::kTempVerify);
    return finish(store, in, Failure::kNone);
  }

  static bool sameConfig(const config_format::Config& a, const config_format::Config& b) {
    return a.tracking_interval_seconds == b.tracking_interval_seconds &&
           a.battery_capacity_mah == b.battery_capacity_mah;
  }

  // Re-arm after the final report has been consumed (kDone only).
  void reset() {
    if (state_ == State::kDone) state_ = State::kIdle;
  }

  // Test hook / diagnostics: a stale unread result was dropped at start().
  bool staleResultDrained() const { return stale_drained_; }

 private:
  bool finish(ConfigStore& store, const Inputs& in, Failure failure) {
    report_.current = store.config();
    report_.ble_connected = in.ble_connected == 1;
    report_.ble_disconnects = in.ble_disconnect_events - start_events_;
    report_.accepted_delta = in.async.async_accepted - start_async_.async_accepted;
    report_.success_delta =
        in.async.completions_success - start_async_.completions_success;
    report_.errors_delta = in.async.completions_error - start_async_.completions_error;
    report_.timeouts_delta = in.async.timeouts - start_async_.timeouts;
    report_.late_delta = in.async.late_completions - start_async_.late_completions;
    report_.failure = failure;
    if (failure == Failure::kNone) {
      // Restore is done and verified; now judge the coexistence evidence.
      if (!report_.ble_connected || report_.ble_disconnects != 0) {
        report_.failure = Failure::kBleLost;
      } else if (report_.accepted_delta == 0 || report_.success_delta == 0 ||
                 report_.success_delta != report_.accepted_delta ||
                 report_.errors_delta != 0 || report_.timeouts_delta != 0 ||
                 report_.late_delta != 0) {
        report_.failure = Failure::kAsyncEvidence;
      }
    }
    // A timeout that leaves the config different from the original is a
    // restore failure too: the exact original is not in effect.
    report_.restore_failure = report_.failure == Failure::kRestoreRequest ||
                              report_.failure == Failure::kRestoreSave ||
                              report_.failure == Failure::kRestoreVerify ||
                              (report_.failure == Failure::kTimeout &&
                               !sameConfig(report_.current, report_.original));
    report_.pass = report_.failure == Failure::kNone;
    state_ = State::kDone;
    return true;
  }

  State state_ = State::kIdle;
  Report report_;
  uint32_t start_events_ = 0;
  AsyncCounters start_async_;
  uint32_t started_ms_ = 0;
  bool stale_drained_ = false;
};

}  // namespace orun_tlp
