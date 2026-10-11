#pragma once

#include <stddef.h>
#include <stdint.h>

#include "geofence_area_set.h"
#include "geofence_format.h"

namespace orun_tlp {

// Non-authoritative candidate for a whole-resource REPLACE_SET
// (ORUN_GEOFENCE_CONFIGURATION_DISTRIBUTION.md sections 3 and 6): vertices
// are collected here, in RAM, one at a time; nothing is validated or
// persisted until the complete set is handed to GeofenceMutationOwner.
// Transport-neutral: USB fills it today, a BLE adapter can later.
//
// Capacity is the durable resource's: 8 areas, 64 vertices in total.
class GeofenceStaging {
 public:
  enum class Result : uint8_t {
    kOk,
    kNotStaging,    // no BEGIN
    kFull,          // 64 vertices already staged
    kEmptyArea,     // AREA while the current area has no vertex
    kTooManyAreas,  // a 9th area
  };

  // Start a new candidate with one empty area; drops any previous one.
  void begin();
  void abort();
  bool active() const { return active_; }

  Result addVertex(const GeoPointE7& point);
  Result nextArea();

  // Areas that have at least one vertex, and vertices in total.
  uint8_t areaCount() const;
  uint16_t vertexCount() const { return total_; }

  // Views over the staged vertices for GeofenceStore validation. Valid only
  // while the staging is unchanged; `polygons` needs kMaximumAreas slots.
  GeofenceAreaSetView view(GeofencePolygonView* polygons) const;

 private:
  bool active_ = false;
  uint8_t areas_ = 0;  // index of the current area + 1
  uint16_t total_ = 0;
  uint8_t counts_[geofence_format::kMaximumAreas] = {};
  GeoPointE7 vertices_[geofence_format::kMaximumTotalVertices];
};

// Decimal degrees with at most 7 decimals ("37.8512345", "-8.5", "28") to an
// exact E7 integer without floating point. Rejects anything else, and a
// magnitude above max_abs_degrees (90 for latitude, 180 for longitude).
bool parseDegreesE7(const char* text, size_t length, int32_t max_abs_degrees,
                    int32_t* value_e7);

}  // namespace orun_tlp
