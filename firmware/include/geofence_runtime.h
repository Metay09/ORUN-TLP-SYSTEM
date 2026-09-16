#pragma once

#include <stdint.h>

#include "geofence_area_set.h"
#include "gnss_fix.h"

namespace orun_tlp {

namespace geofence_runtime_config {

// M6C3 runtime resource budget only. The budget is expressed in effective
// polygon vertices: an optional final vertex equal to the first is canonicalized
// away when the runtime takes ownership. This is not a durable/public product
// configuration limit.
constexpr uint8_t kMaximumAreas = 8;
constexpr uint16_t kMaximumTotalVertices = 64;

}  // namespace geofence_runtime_config

enum class GeofenceRuntimeConfigResult : uint8_t {
  kApplied,
  kInvalidAreaSet,
  kTooManyAreas,
  kTooManyVertices,
};

enum class GeofenceObservationResult : uint8_t {
  kNotConfigured,
  kAccepted,
  kInvalidPoint,
  kConfigFault,
};

// Runtime-only owner for one bounded copy of permitted geofence geometry and
// the latest valid local assessment. It deliberately does not own GNSS
// acquisition/quality/freshness, location-source arbitration, persistence,
// hysteresis, NEAR_FENCE, FREE_GRAZE, LOST, RF events or alarms.
class GeofenceRuntime {
 public:
  GeofenceRuntime() = default;
  GeofenceRuntime(const GeofenceRuntime&) = delete;
  GeofenceRuntime& operator=(const GeofenceRuntime&) = delete;
  GeofenceRuntime(GeofenceRuntime&&) = delete;
  GeofenceRuntime& operator=(GeofenceRuntime&&) = delete;

  // Candidate replacement is all-or-nothing: invalid/oversized candidates are
  // rejected without changing the previously applied runtime configuration or
  // its last valid assessment.
  GeofenceRuntimeConfigResult configure(const GeofenceAreaSetView& candidate);
  void clear();

  bool configured() const { return configured_; }
  uint8_t areaCount() const { return area_count_; }
  uint16_t totalVertexCount() const { return total_vertex_count_; }

  // The caller must supply a fix already accepted by the location/tracking
  // owner. M6C3 does not create a competing freshness, HDOP or source policy.
  GeofenceObservationResult observeAcceptedFix(const GnssFix& fix);

  bool hasAssessment() const { return has_assessment_; }
  const PermittedAreaAssessment& assessment() const { return assessment_; }
  uint32_t assessedFixCapturedAtMs() const { return assessed_fix_captured_at_ms_; }

 private:
  GeoPointE7 vertices_[geofence_runtime_config::kMaximumTotalVertices]{};
  GeofencePolygonView areas_[geofence_runtime_config::kMaximumAreas]{};
  uint8_t area_count_ = 0;
  uint16_t total_vertex_count_ = 0;
  bool configured_ = false;
  bool has_assessment_ = false;
  PermittedAreaAssessment assessment_{};
  uint32_t assessed_fix_captured_at_ms_ = 0;
};

}  // namespace orun_tlp
