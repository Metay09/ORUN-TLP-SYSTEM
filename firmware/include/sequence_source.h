#pragma once
#include <stdint.h>
namespace orun_tlp {
class SequenceSource {
 public:
  virtual ~SequenceSource() = default;
  // Identity is a nonzero 64-bit ticket; wire sequence = low32(identity - 1).
  virtual bool nextSequence(uint32_t& sequence, uint64_t& identity) = 0;
};
}  // namespace orun_tlp
