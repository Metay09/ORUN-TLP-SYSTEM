#pragma once

#include <stdint.h>

#include "application_request.h"
#include "config_mutation.h"
#include "geofence_area_set.h"
#include "geofence_store.h"

namespace orun_tlp {

// The one owner of geofence resource changes
// (ORUN_GEOFENCE_CONFIGURATION_DISTRIBUTION.md section 6): whole-resource
// REPLACE_SET and CLEAR_SET only, serialized in one slot, durably committed by
// GeofenceStore before any runtime effect, then reported with the outcome
// vocabulary of ConfigMutationOwner. The geofence keeps its own state token
// (GeofenceStore); it does not share the ConfigStore token.
//
// Admits the local USB channel only (physical-access trust, like the config
// writer). BLE needs reviewed application authorization and LoRa the
// protected TLP v2 path first; both will submit to this same owner.
enum class GeofenceMutationKind : uint8_t {
  kReplace = 1,
  kClear = 2,
};

struct GeofenceMutationResult {
  GeofenceMutationResult()
      : requester(ApplicationRequester::kUsb),
        request_id(0),
        kind(GeofenceMutationKind::kClear),
        outcome(ConfigMutationOutcome::kUnavailable),
        state(GeofenceResourceState::kUnavailable),
        area_count(0),
        total_vertex_count(0),
        token_valid(false),
        token() {}

  ApplicationRequester requester;
  uint32_t request_id;
  GeofenceMutationKind kind;
  ConfigMutationOutcome outcome;
  // What the store holds when the operation ended, not what was asked for.
  GeofenceResourceState state;
  uint8_t area_count;
  uint16_t total_vertex_count;
  bool token_valid;
  geofence_format::StateToken token;
};

class GeofenceMutationOwner {
 public:
  explicit GeofenceMutationOwner(GeofenceStore& store)
      : store_(store), state_(State::kIdle), result_() {}

  // `area_set` is validated and copied by the store during the call.
  ConfigMutationSubmitResult submitReplace(ApplicationRequester requester,
                                           uint32_t request_id,
                                           ApplicationAccessContext access,
                                           const GeofenceAreaSetView& area_set);
  ConfigMutationSubmitResult submitClear(ApplicationRequester requester,
                                         uint32_t request_id,
                                         ApplicationAccessContext access);

  // Call once per loop pass, after GeofenceStore::poll(). Returns true
  // exactly once per durably committed change, in the pass where its result
  // becomes takeable, so the runtime can follow before the result is shown.
  bool poll();

  bool takeResult(ApplicationRequester requester,
                  GeofenceMutationResult& result);

  bool busy() const { return state_ != State::kIdle; }

 private:
  enum class State : uint8_t { kIdle, kSaving, kResultReady };

  ConfigMutationSubmitResult submit(ApplicationRequester requester,
                                    uint32_t request_id,
                                    ApplicationAccessContext access,
                                    GeofenceMutationKind kind,
                                    const GeofenceAreaSetView* area_set);
  void complete(ConfigMutationOutcome outcome);

  GeofenceStore& store_;
  State state_;
  GeofenceMutationResult result_;
};

}  // namespace orun_tlp
