#pragma once
#include <stdint.h>

namespace orun_tlp {

class ObservationIncarnationSource {
 public:
  virtual ~ObservationIncarnationSource() = default;
  virtual bool generate(uint64_t& value) = 0;
};

}  // namespace orun_tlp
