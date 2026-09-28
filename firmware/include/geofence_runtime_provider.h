#pragma once

#include <stdint.h>

#include "geofence_confirmation_coordinator.h"
#include "geofence_format.h"

namespace orun_tlp {

// M6D3C: narrow semantic bridge from one already-recovered durable geofence
// snapshot into the existing M6D2 runtime owner.
//
// This is not a persistence owner, transport, mutation service or GNSS owner.
// GeofenceRuntime copies the supplied geometry during configure(), so the
// temporary polygon views created by the implementation never escape.
enum class GeofenceRuntimeApplyResult : uint8_t {
  kCleared,
  kConfigured,
  kRejected,
};

GeofenceRuntimeApplyResult applyGeofenceSnapshotToRuntime(
    const geofence_format::Snapshot& snapshot,
    GeofenceConfirmationCoordinator& coordinator);

}  // namespace orun_tlp
