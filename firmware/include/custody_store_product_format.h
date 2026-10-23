#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace custody_store_product_format {

// SF5K3O: new PRODUCT_SECURE custody page candidate, distinct from legacy
// OCQ1/v2 73-byte HISTORY_SECURE storage. Portable format only: this module
// MUST NOT be treated as an activated Gateway store or ACK authority.
constexpr uint32_t kPageSize = 4096U;
constexpr uint32_t kPageMagic = 0x4F435132U; // OCQ2, not legacy OCQ1
constexpr uint16_t kVersion = 3U;
constexpr uint32_t kPageHeaderSize = 64U;
constexpr uint32_t kRecordSize = 120U;
constexpr size_t kMaxObjectSize = 100U;
constexpr size_t kPeriodicSize = 100U;
constexpr size_t kEventSize = 84U;
constexpr uint32_t kIntentSlotsPerPage = 3U;
constexpr uint32_t kIntentSlotSize = 24U;
constexpr uint32_t kIntentAreaSize = kIntentSlotsPerPage * kIntentSlotSize;
constexpr uint32_t kIntentAreaOffset = kPageSize - kIntentAreaSize;
constexpr uint32_t kRecordsPerPage =
    (kIntentAreaOffset - kPageHeaderSize) / kRecordSize;
constexpr uint32_t kObjectLengthOffset = 100U;
constexpr uint32_t kRecordCrcOffset = 104U;
constexpr uint32_t kRecordCommitOffset = 108U;
constexpr uint32_t kRecordHandoff0Offset = 112U;
constexpr uint32_t kRecordHandoff1Offset = 116U;
constexpr uint32_t kHeaderCrcOffset = 28U;
constexpr uint32_t kHeaderCommitOffset = 32U;
constexpr uint32_t kHeaderActiveOffset = 36U;
static_assert(kRecordsPerPage == 33U, "SF5K3O page geometry changed");
static_assert(kPageHeaderSize + kRecordsPerPage * kRecordSize +
                  kIntentAreaSize == kPageSize, "SF5K3O page must fit exactly");

// Neither CRC nor structural validity authenticates origin. The Gateway must
// separately admit sources and durably provision scoped custody ACK authority.
enum class PageEvidence : uint8_t {
  kErased, kStaged, kPartialCommit, kPrepared, kPartialActivation,
  kActive, kCorrupt, kUnsupported,
};
enum class RecordEvidence : uint8_t {
  kErased, kStaged, kPartialCommit, kHeld, kHandedOff, kCorrupt,
};
struct PageInspection {
  PageEvidence evidence = PageEvidence::kCorrupt;
  uint64_t generation = 0U;
};
struct RecordInspection {
  RecordEvidence evidence = RecordEvidence::kCorrupt;
  uint8_t object_size = 0U;
  uint8_t object[kMaxObjectSize]{};
  bool handoff_uncertain = false;
  uint8_t next_handoff_slot = UINT8_MAX;
};

// Encoding refuses invalid lengths. Caller output is unchanged on rejection.
bool encodePageHeader(uint64_t generation, uint8_t (&out)[kPageHeaderSize]);
bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection);
bool encodeRecord(const uint8_t* object, size_t size,
                  uint8_t (&out)[kRecordSize]);
bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection);
bool exactObject(const RecordInspection& record, const uint8_t* object,
                 size_t size);

} // namespace custody_store_product_format
} // namespace orun_tlp
