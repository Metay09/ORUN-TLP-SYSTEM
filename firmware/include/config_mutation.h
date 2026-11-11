#pragma once

#include <stddef.h>
#include <stdint.h>

#include "application_request.h"
#include "config_store.h"

namespace orun_tlp {

// The one application-level owner of durable configuration changes.
//
// Every transport that changes desired configuration (USB today; BLE, LoRa and
// backend adapters later) submits here. It serializes one change at a time,
// holds the single mutation slot from admission until the result is taken,
// and reports a typed outcome. See
// docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md section 3.4 and
// docs/architecture/ORUN_APPLICATION_TRANSPORT_SURFACE.md section 7.
//
// Admits local USB and (4c-1b) account-authenticated BLE writes
// (ble_write_service.h). A caller-supplied precondition token (CAS) and the
// LoRa transport are later slices that reuse this owner; nothing here is a
// wire format.
//
// Loop-owned and fixed-memory, like the ConfigStore it drives.

namespace config_mutation_policy {
// Writer-side floor for a requested tracking interval. One POSITION is about
// one second of SF11 airtime and confirmed OUTSIDE runs at a third of the
// base interval, so a shorter base would let a single setting saturate the
// channel and the battery. This is an admission guard for user writes, not a
// regulatory claim; ConfigStore's own semantic bounds are unchanged, so
// existing records are never reclassified by it.
constexpr uint32_t kMinTrackingIntervalSeconds = 60;
}  // namespace config_mutation_policy

enum class ConfigMutationKind : uint8_t {
  kSetTrackingInterval = 1,
  // service_mode + requested_services (ConfigStore v4). Only combinations
  // this runtime can run are admitted (service_intent.h).
  kSetServiceIntent = 2,
  // Seconds Bluetooth stays open after each report (ConfigStore v8), 0 = off.
  kSetBleWindow = 3,
};

struct ConfigMutationRequest {
  constexpr ConfigMutationRequest(
      ApplicationRequester requester_value,
      uint32_t request_id_value,
      ApplicationAccessContext access_value,
      ConfigMutationKind kind_value,
      uint32_t tracking_interval_seconds_value,
      uint8_t service_mode_value = 0,
      uint8_t requested_services_value = 0,
      uint32_t ble_window_seconds_value = 0)
      : requester(requester_value),
        request_id(request_id_value),
        access(access_value),
        kind(kind_value),
        tracking_interval_seconds(tracking_interval_seconds_value),
        service_mode(service_mode_value),
        requested_services(requested_services_value),
        ble_window_seconds(ble_window_seconds_value) {}

  ApplicationRequester requester;
  uint32_t request_id;
  ApplicationAccessContext access;
  ConfigMutationKind kind;
  // kSetTrackingInterval
  uint32_t tracking_interval_seconds;
  // kSetServiceIntent
  uint8_t service_mode;
  uint8_t requested_services;
  // kSetBleWindow
  uint32_t ble_window_seconds;
};

enum class ConfigMutationSubmitResult : uint8_t {
  // The mutation slot is now held; exactly one result will become takeable.
  kAccepted = 0,
  // The slot is held by another change, or the store is finishing other
  // work. Nothing was evaluated; retry later.
  kBusy = 1,
  // Unknown requester or an access context that cannot belong to it.
  kRejected = 2,
};

enum class ConfigMutationOutcome : uint8_t {
  // Durably committed, read back and published. `token` is the new state.
  kApplied = 0,
  // Desired state already in effect. Nothing written; `token` unchanged.
  kUnchanged = 1,
  // The requested value is outside the admitted range, or the service
  // combination cannot run on this firmware.
  kInvalid = 2,
  // This access channel may not change configuration (yet).
  kAccessDenied = 3,
  // The store needs maintenance/reset or has no VALID state token.
  kMaintenance = 4,
  // The store is not ready or is reconciling an earlier mutation.
  kUnavailable = 5,
  // The save attempt was not confirmed. It MAY have committed: re-read the
  // configuration after the store reconciles. Never report this as FAILED.
  kOutcomeUnknown = 6,
  // 6 (LoRa): the command was signed for an older saved state (replayed, or
  // overtaken by another change). Nothing written; `token` is the current
  // state, so the server can sign again against it.
  kStale = 7,
};

struct ConfigMutationResult {
  ConfigMutationResult()
      : requester(ApplicationRequester::kUsb),
        request_id(0),
        kind(ConfigMutationKind::kSetTrackingInterval),
        outcome(ConfigMutationOutcome::kUnavailable),
        token_valid(false),
        token(),
        config() {}

  ApplicationRequester requester;
  uint32_t request_id;
  ConfigMutationKind kind;
  ConfigMutationOutcome outcome;
  // Authoritative store state captured when the operation ended. This is
  // what the store holds, not the value that was asked for.
  bool token_valid;
  config_format::StateToken token;
  config_format::Config config;
};

class ConfigMutationOwner {
 public:
  explicit ConfigMutationOwner(ConfigStore& store)
      : store_(store), state_(State::kIdle), result_() {}

  ConfigMutationSubmitResult submit(const ConfigMutationRequest& request);

  // 6 (LoRa): several settings in ONE save, all or none (same requester,
  // request id and access for every item). With `expected`, only if the
  // store's state token is still exactly that one; otherwise kStale.
  static constexpr size_t kMaxBatch = 8;
  ConfigMutationSubmitResult submitBatch(const ConfigMutationRequest* items, size_t count,
                                         const config_format::StateToken* expected);

  // Call once per loop pass, after ConfigStore::poll(). Returns true exactly
  // once for each durably committed change, in the pass where its result
  // becomes takeable, so the composition root can apply the new
  // configuration to the runtime before any adapter reports the result.
  bool poll();

  bool takeResult(ApplicationRequester requester, ConfigMutationResult& result);

  bool busy() const { return state_ != State::kIdle; }
  bool resultPending() const { return state_ == State::kResultReady; }

 private:
  enum class State : uint8_t { kIdle, kSaving, kResultReady };

  void complete(ConfigMutationOutcome outcome);

  ConfigStore& store_;
  State state_;
  ConfigMutationResult result_;
};

}  // namespace orun_tlp
