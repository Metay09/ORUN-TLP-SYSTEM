#include <assert.h>
#include <stdio.h>

#include "geofence_format.h"
#include "geofence_runtime_provider.h"

using namespace orun_tlp;

namespace {

geofence_format::Snapshot configuredSnapshot(
    const GeofencePolygonView* polygons, uint16_t count) {
  geofence_format::Snapshot snapshot;
  assert(geofence_format::canonicalizeConfiguredAreaSet(
      GeofenceAreaSetView(polygons, count), snapshot));
  return snapshot;
}

}  // namespace

int main() {
  const GeoPointE7 square_a[] = {
      GeoPointE7(10000000, 20000000),
      GeoPointE7(10000000, 20010000),
      GeoPointE7(10010000, 20010000),
      GeoPointE7(10010000, 20000000),
  };
  const GeoPointE7 square_b[] = {
      GeoPointE7(11000000, 21000000),
      GeoPointE7(11000000, 21010000),
      GeoPointE7(11010000, 21010000),
      GeoPointE7(11010000, 21000000),
  };

  // 1. Durable CLEAR explicitly disables runtime geofence evaluation.
  {
    GeofenceConfirmationCoordinator coordinator;
    const GeofencePolygonView polygon(square_a, 4);
    auto configured = configuredSnapshot(&polygon, 1);
    assert(applyGeofenceSnapshotToRuntime(configured, coordinator) ==
           GeofenceRuntimeApplyResult::kConfigured);
    assert(coordinator.configured());

    geofence_format::Snapshot clear;
    geofence_format::makeClearSnapshot(clear);
    assert(applyGeofenceSnapshotToRuntime(clear, coordinator) ==
           GeofenceRuntimeApplyResult::kCleared);
    assert(!coordinator.configured());
    assert(!coordinator.confirmationActive());
    assert(coordinator.cadenceMode() == GeofenceCadenceMode::kBase);
  }

  // 2. Flattened durable multi-area geometry is reconstructed exactly for the
  // existing M6D2 owner; the coordinator takes its own bounded copy.
  {
    GeofenceConfirmationCoordinator coordinator;
    const GeofencePolygonView polygons[] = {
        GeofencePolygonView(square_a, 4),
        GeofencePolygonView(square_b, 4),
    };
    auto snapshot = configuredSnapshot(polygons, 2);
    assert(snapshot.area_count == 2);
    assert(snapshot.total_vertex_count == 8);
    assert(applyGeofenceSnapshotToRuntime(snapshot, coordinator) ==
           GeofenceRuntimeApplyResult::kConfigured);
    assert(coordinator.configured());

    const auto inside_a = coordinator.observeAcceptedLocation(
        GeoPointE7(10005000, 20005000), 1000, 100, 8);
    assert(inside_a.geometry_result == GeofenceObservationResult::kAccepted);
    assert(inside_a.operational_result ==
           GeofenceOperationalResult::kInitializedInside);
    assert(!inside_a.outside_event_occurrence);
  }

  // 3. Replacing geometry resets evidence/cadence authority tied to the old
  // snapshot. It must not synthesize an OUTSIDE event from configuration
  // change alone.
  {
    GeofenceConfirmationCoordinator coordinator;
    const GeofencePolygonView first(square_a, 4);
    auto first_snapshot = configuredSnapshot(&first, 1);
    assert(applyGeofenceSnapshotToRuntime(first_snapshot, coordinator) ==
           GeofenceRuntimeApplyResult::kConfigured);

    const auto inside = coordinator.observeAcceptedLocation(
        GeoPointE7(10005000, 20005000), 1000, 100, 8);
    assert(inside.operational_result ==
           GeofenceOperationalResult::kInitializedInside);

    const auto outside_candidate = coordinator.observeAcceptedLocation(
        GeoPointE7(12000000, 22000000), 2000, 100, 8);
    assert(outside_candidate.operational_result ==
           GeofenceOperationalResult::kConfirmationStarted);
    assert(coordinator.confirmationActive());

    const GeofencePolygonView second(square_b, 4);
    auto second_snapshot = configuredSnapshot(&second, 1);
    assert(applyGeofenceSnapshotToRuntime(second_snapshot, coordinator) ==
           GeofenceRuntimeApplyResult::kConfigured);
    assert(coordinator.configured());
    assert(!coordinator.confirmationActive());
    assert(coordinator.cadenceMode() == GeofenceCadenceMode::kBase);

    const auto fresh = coordinator.observeAcceptedLocation(
        GeoPointE7(11005000, 21005000), 3000, 100, 8);
    assert(fresh.operational_result ==
           GeofenceOperationalResult::kInitializedInside);
    assert(!fresh.outside_event_occurrence);
  }

  // 4. Structurally impossible durable input is rejected without replacing an
  // already-applied runtime configuration.
  {
    GeofenceConfirmationCoordinator coordinator;
    const GeofencePolygonView polygon(square_a, 4);
    auto valid = configuredSnapshot(&polygon, 1);
    assert(applyGeofenceSnapshotToRuntime(valid, coordinator) ==
           GeofenceRuntimeApplyResult::kConfigured);

    auto malformed = valid;
    malformed.total_vertex_count = 2;
    assert(applyGeofenceSnapshotToRuntime(malformed, coordinator) ==
           GeofenceRuntimeApplyResult::kRejected);
    assert(coordinator.configured());

    const auto still_inside = coordinator.observeAcceptedLocation(
        GeoPointE7(10005000, 20005000), 4000, 100, 8);
    assert(still_inside.geometry_result == GeofenceObservationResult::kAccepted);
  }

  puts("M6D3C durable snapshot -> M6D2 runtime provider checks: PASS");
}
