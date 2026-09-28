#pragma once
#include <stdint.h>

namespace orun_tlp {

// Supplies one fresh nonzero GeofenceStore incarnation identifier.
//
// This is intentionally separate from ConfigStore's token namespace. The token
// is not secret, but each fresh geofence baseline/re-baseline must get an
// unpredictable namespace identifier so stale cached CAS tokens cannot collide
// across lifetimes.
class GeofenceIncarnationSource {
 public:
  virtual ~GeofenceIncarnationSource() = default;
  virtual bool generate(uint64_t& incarnation) = 0;
};

// RAK4630/nRF52840 production source. Current M6D3B use is pre-SoftDevice only.
// A later runtime re-baseline while SoftDevice is active requires its own
// reviewed entropy path; never touch the raw RNG peripheral after SD owns it.
class NrfGeofenceIncarnationSource : public GeofenceIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override;
};

}  // namespace orun_tlp
