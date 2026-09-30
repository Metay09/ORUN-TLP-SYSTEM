#include "geofence_runtime_provider.h"

namespace orun_tlp {

GeofenceRuntimeApplyResult applyGeofenceSnapshotToRuntime(
    const geofence_format::Snapshot& snapshot,
    GeofenceConfirmationCoordinator& coordinator) {
  using geofence_format::ResourceState;

  if (snapshot.state == ResourceState::kClear) {
    // Durable CLEAR is an explicit semantic state, not "missing bytes".
    // Require its canonical empty shape before changing runtime authority.
    if (snapshot.area_count != 0 || snapshot.total_vertex_count != 0)
      return GeofenceRuntimeApplyResult::kRejected;
    for (uint8_t area = 0; area < geofence_format::kMaximumAreas; ++area)
      if (snapshot.area_vertex_counts[area] != 0)
        return GeofenceRuntimeApplyResult::kRejected;

    coordinator.clear();
    return GeofenceRuntimeApplyResult::kCleared;
  }

  if (snapshot.state != ResourceState::kConfigured ||
      snapshot.area_count == 0 ||
      snapshot.area_count > geofence_format::kMaximumAreas ||
      snapshot.total_vertex_count == 0 ||
      snapshot.total_vertex_count > geofence_format::kMaximumTotalVertices) {
    return GeofenceRuntimeApplyResult::kRejected;
  }

  GeofencePolygonView areas[geofence_format::kMaximumAreas]{};
  uint16_t offset = 0;

  for (uint8_t area = 0; area < snapshot.area_count; ++area) {
    const uint8_t count = snapshot.area_vertex_counts[area];
    if (count < 3 ||
        offset > snapshot.total_vertex_count ||
        count > snapshot.total_vertex_count - offset) {
      return GeofenceRuntimeApplyResult::kRejected;
    }

    areas[area] = GeofencePolygonView(snapshot.vertices + offset, count);
    offset = static_cast<uint16_t>(offset + count);
  }

  if (offset != snapshot.total_vertex_count) {
    return GeofenceRuntimeApplyResult::kRejected;
  }

  for (uint8_t area = snapshot.area_count;
       area < geofence_format::kMaximumAreas; ++area) {
    if (snapshot.area_vertex_counts[area] != 0)
      return GeofenceRuntimeApplyResult::kRejected;
  }

  const GeofenceRuntimeConfigResult result = coordinator.configure(
      GeofenceAreaSetView(areas, snapshot.area_count));
  return result == GeofenceRuntimeConfigResult::kApplied
             ? GeofenceRuntimeApplyResult::kConfigured
             : GeofenceRuntimeApplyResult::kRejected;
}

}  // namespace orun_tlp
