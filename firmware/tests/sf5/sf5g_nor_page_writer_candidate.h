#pragma once

// SF5G host-only single-page NOR durability MODEL.
// No production FlashBackend, mount/owner selection, erase/reclaim, radio or ACK.
#include <stddef.h>
#include <stdint.h>

#include <array>

#include "sf5g_custody_record_candidate.h"
#include "sf5g_page_header_candidate.h"

namespace orun_tlp {
namespace sf5g_nor_writer_candidate {

namespace rec = sf5g_custody_candidate;
namespace page = sf5g_page_candidate;

// Persist this object across simulated resets; replace PageWriter, not NorPage.
// Programming allows only 1->0 bits; power failure can stop after *any byte*.
// There is deliberately NO erase API.
class NorPage {
 public:
  NorPage();

  // All instrumentation changes simulation behavior, not flash bytes.
  void setProgramByteBudget(size_t budget);
  void setReadBudget(size_t reads);
  void clearFaults();
  bool powerLost() const { return power_lost_; }
  size_t programmedBytes() const { return programmed_bytes_; }

  bool read(size_t offset, uint8_t* out, size_t size) const;
  bool program(size_t offset, const uint8_t* data, size_t size);

 private:
  std::array<uint8_t, page::kPageSize> bytes_{};
  size_t program_budget_ = SIZE_MAX;
  size_t programmed_bytes_ = 0U;
  mutable size_t read_budget_ = SIZE_MAX;
  bool power_lost_ = false;
};

enum class WriteResult : uint8_t {
  kInitialized,
  kStored,
  kDuplicate,
  kFull,
  kRefused,
  kReadFailure,
  kPowerCut,
};

enum class PageState : uint8_t {
  kErased,
  kActive,
  kRefused,
  kReadFailure,
};

struct PageReport {
  PageState state = PageState::kRefused;
  uint64_t generation = 0U;
  uint16_t held = 0U;
  uint16_t handed_off = 0U;
  uint16_t first_erased_slot = UINT16_MAX;
};

// A *single-page*, strictly ordered prototype. It cannot issue custody
// acknowledgments and does not authenticate sealed objects (CRC is not AEAD).
// Never use as a production Gateway implementation.
class PageWriter {
 public:
  explicit PageWriter(NorPage& nor) : nor_(nor) {}

  // Explicit caller consent AND a completely erased 4 KiB page are required.
  // A staged, unknown, foreign or torn page is never reformatted/retried.
  WriteResult initializeEmpty(uint64_t generation, bool authorized);
  WriteResult appendExact(const uint8_t* object, size_t object_size);
  PageReport inspect() const;

 private:
  NorPage& nor_;
  bool readExact(size_t offset, uint8_t* out, size_t size) const;
  bool reservedAreasErased(const uint8_t* whole_page) const;
};

}  // namespace sf5g_nor_writer_candidate
}  // namespace orun_tlp
