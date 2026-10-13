#pragma once

// Host-only SF5G multi-page, NO-ERASE progression prototype.
// No production owner/mount integration, flash erase, reclaim or custody ACK.
#include <stddef.h>
#include <stdint.h>

#include <array>

#include "sf5g_nor_page_writer_candidate.h"

namespace orun_tlp {
namespace sf5g_region_candidate {

namespace nor = sf5g_nor_writer_candidate;
namespace page = sf5g_page_candidate;
namespace rec = sf5g_custody_candidate;

// Owns up to 32 simulated NOR pages and survives a reconstructed RegionWriter.
// NorPage itself has no erase API. This is not the on-device FlashBackend.
class RegionMedia {
 public:
  explicit RegionMedia(size_t page_count) : page_count_(page_count) {}
  bool valid() const {
    return page_count_ > 0U && page_count_ <= page::kMaxRegionPages;
  }
  size_t pageCount() const { return page_count_; }
  nor::NorPage* pageAt(size_t index) {
    return valid() && index < page_count_ ? &pages_[index] : nullptr;
  }
  const nor::NorPage* pageAt(size_t index) const {
    return valid() && index < page_count_ ? &pages_[index] : nullptr;
  }

 private:
  std::array<nor::NorPage, page::kMaxRegionPages> pages_{};
  size_t page_count_;
};

enum class RegionState : uint8_t {
  kErased,
  kActive,
  kRefused,
  kReadFailure,
};

struct RegionReport {
  RegionState state = RegionState::kRefused;
  uint16_t active_pages = 0U;
  uint16_t newest_page = UINT16_MAX;
  uint16_t newest_first_erased_slot = UINT16_MAX;
  uint16_t held = 0U;
  uint16_t handed_off = 0U;
  uint64_t newest_generation = 0U;
};

// The caller must pass explicit owner/provisioning authorization for each
// mutation. A complete region census always runs first. Existing pages
// must form a contiguous physical prefix, with unique generations 1..N;
// every older page must be full before a newer ACTIVE page is admitted.
// No compaction/reclamation is possible in this limited prototype.
class RegionWriter {
 public:
  explicit RegionWriter(RegionMedia& media) : media_(media) {}

  RegionReport inspect() const;
  nor::WriteResult initializeEmpty(bool owner_authorized);
  nor::WriteResult appendExact(const uint8_t* object, size_t size,
                               bool owner_authorized);

 private:
  RegionReport inspectAndFind(const uint8_t* object, size_t size,
                              bool* duplicate) const;
  RegionMedia& media_;
};

}  // namespace sf5g_region_candidate
}  // namespace orun_tlp
