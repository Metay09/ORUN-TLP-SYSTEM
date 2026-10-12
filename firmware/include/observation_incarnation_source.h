#pragma once
#include <stdint.h>

namespace orun_tlp {

class ObservationIncarnationSource {
 public:
  virtual ~ObservationIncarnationSource() = default;
  virtual bool generate(uint64_t& value) = 0;
};

// RAK4630/nRF52840 production source: the hardware RNG, pre-SoftDevice only
// (the store mounts before Bluefruit.begin()). A later in-runtime re-baseline
// while SoftDevice owns the RNG needs its own reviewed entropy path.
class NrfObservationIncarnationSource : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override;
};

}  // namespace orun_tlp
