#pragma once

#include <stdint.h>

#include "application_status.h"
#include "config_store.h"
#include "geofence_confirmation_coordinator.h"
#include "geofence_store.h"
#include "gnss_manager.h"
#include "history_store.h"
#include "node_role.h"
#include "runtime_config.h"
#include "security_store.h"

namespace orun_tlp {

// M7P7H production-composition helper. This is deliberately concrete rather
// than a generic StatusProvider framework: it reads the owners that exist
// today and builds the bounded transport-neutral snapshot. It owns no state,
// performs no I/O, scans no history records and copies no geofence geometry.
void buildApplicationStatusSnapshot(
    uint32_t now_ms,
    NodeRole role,
    bool role_automatic,
    const CapabilitySnapshot& capabilities,
    const EffectiveConfig& effective,
    bool relay_forwarding_applied,
    uint32_t applied_base_interval_seconds,
    const GnssManager& gnss,
    const ConfigStore& config,
    const GeofenceStore& geofence_store,
    const GeofenceConfirmationCoordinator& geofence_runtime,
    const HistoryStore& history,
    const SecurityStore& security,
    ApplicationStatusSnapshot& out);

}  // namespace orun_tlp
