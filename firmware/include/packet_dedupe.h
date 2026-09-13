#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

struct PacketKey {
  uint64_t source_device_id = 0;
  uint32_t sequence_number = 0;
};

inline bool operator==(const PacketKey& left, const PacketKey& right) {
  return left.source_device_id == right.source_device_id &&
         left.sequence_number == right.sequence_number;
}

template <size_t Capacity>
class PacketDedupeCache {
 public:
  bool contains(const PacketKey& key) const {
    for (size_t index = 0; index < Capacity; ++index) {
      if (valid_[index] && entries_[index] == key) return true;
    }
    return false;
  }

  bool observe(const PacketKey& key) {
    if (contains(key)) return true;
    entries_[next_] = key;
    valid_[next_] = true;
    next_ = (next_ + 1) % Capacity;
    return false;
  }

  void clear() {
    for (size_t index = 0; index < Capacity; ++index) valid_[index] = false;
    next_ = 0;
  }

 private:
  PacketKey entries_[Capacity]{};
  bool valid_[Capacity]{};
  size_t next_ = 0;
};

}  // namespace orun_tlp
