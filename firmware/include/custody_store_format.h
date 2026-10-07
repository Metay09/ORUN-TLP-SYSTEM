#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_v2_history_secure.h"

namespace orun_tlp {
namespace custody_store_format {

constexpr uint32_t kPageSize = 4096U;
constexpr uint32_t kPageMagic = 0x4F435131U;  // "OCQ1"
constexpr uint16_t kVersion = 2U;
constexpr uint32_t kPageHeaderSize = 64U;
constexpr uint32_t kRecordSize = 92U;
constexpr size_t kObjectSize = tlp::kHistoryObservationPacketSize;

constexpr uint32_t kIntentSlotSize = 24U;
constexpr uint32_t kIntentSlotsPerPage = 3U;
constexpr uint32_t kIntentAreaSize =
    kIntentSlotSize * kIntentSlotsPerPage;
constexpr uint32_t kIntentAreaOffset = kPageSize - kIntentAreaSize;

constexpr uint32_t kRecordsPerPage =
    (kIntentAreaOffset - kPageHeaderSize) / kRecordSize;
constexpr uint32_t kRecordAreaEnd =
    kPageHeaderSize + kRecordsPerPage * kRecordSize;
constexpr uint32_t kRecordIntentGapSize =
    kIntentAreaOffset - kRecordAreaEnd;

constexpr uint32_t kPageStaticCrcOffset = 28U;
constexpr uint32_t kPageHeaderCommitOffset = 32U;
constexpr uint32_t kPageHeaderActiveOffset = 36U;

constexpr uint32_t kRecordCrcOffset = 76U;
constexpr uint32_t kRecordCommitOffset = 80U;
constexpr uint32_t kRecordHandoff0Offset = 84U;
constexpr uint32_t kRecordHandoff1Offset = 88U;
constexpr uint32_t kRecordHandoffSlots = 2U;

constexpr uint32_t kIntentCrcOffset = 12U;
constexpr uint32_t kIntentCommitOffset = 16U;
constexpr uint32_t kIntentCompleteOffset = 20U;

constexpr uint32_t kCommit = 0U;
constexpr uint32_t kActive = 0U;
constexpr uint32_t kHandedOff = 0U;
constexpr uint32_t kIntentCompleted = 0U;

static_assert(kObjectSize == 73U,
              "SF4B storage format is bound to current 73-byte HISTORY_SECURE observation");
static_assert(kRecordsPerPage == 43U, "SF4B v2 page capacity changed");
static_assert(kRecordIntentGapSize == 4U, "SF4B v2 record/intent gap changed");
static_assert(kIntentAreaOffset + kIntentAreaSize == kPageSize,
              "SF4B v2 intent area must end at page boundary");

uint32_t crc32(const uint8_t* data, size_t size);
void put16(uint8_t* data, uint16_t value);
void put32(uint8_t* data, uint32_t value);
void put64(uint8_t* data, uint64_t value);
uint16_t get16(const uint8_t* data);
uint32_t get32(const uint8_t* data);
uint64_t get64(const uint8_t* data);
bool erased(const uint8_t* data, size_t size);

enum class PageEvidence : uint8_t {
  kErased,
  kPrepared,
  kActive,
  kStaged,
  kPartialCommit,
  kPartialActivation,
  kCorrupt,
  kUnsupported,
};

struct PageInspection {
  PageEvidence evidence = PageEvidence::kCorrupt;
  uint64_t generation = 0;
};

enum class ReclaimEvidence : uint8_t {
  kNone,
  kStaged,
  kPartialCommit,
  kCommitted,
  kCompleted,
  kPartialCompletion,
  kCorrupt,
};

struct ReclaimInspection {
  ReclaimEvidence evidence = ReclaimEvidence::kNone;
  uint16_t target_page = UINT16_MAX;
  uint64_t target_generation = 0;
};

enum class RecordEvidence : uint8_t {
  kErased,
  kHeld,
  kHandedOff,
  kStaged,
  kPartialCommit,
  kCorrupt,
};

struct RecordInspection {
  RecordEvidence evidence = RecordEvidence::kCorrupt;
  bool handoff_uncertain = false;
  uint8_t next_handoff_slot = UINT8_MAX;
  uint8_t object[kObjectSize]{};
};

void encodePageHeader(uint64_t generation, uint8_t* bytes);
bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection);

void encodeReclaimIntent(uint16_t target_page, uint64_t target_generation,
                         uint8_t* bytes);
bool inspectReclaimIntent(const uint8_t* bytes, size_t size,
                          ReclaimInspection& inspection);

void encodeRecord(const uint8_t* object, size_t object_size, uint8_t* bytes);
bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection);
bool exactObject(const RecordInspection& inspection,
                 const uint8_t* object, size_t object_size);

}  // namespace custody_store_format
}  // namespace orun_tlp
