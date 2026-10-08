#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace observation_store_format {

constexpr uint32_t kPageSize = 4096U;
constexpr uint32_t kPageMagic = 0x4F425331U;  // "OBS1"
constexpr uint16_t kVersion = 1U;
constexpr uint32_t kPageHeaderSize = 64U;

constexpr uint32_t kDataRecordSize = 96U;
constexpr uint32_t kDataPayloadSize = 64U;

// SF5B schema-v1 semantic plaintext sizes. ObservationStore persists the
// canonical product record, not an arbitrarily truncated payload.
constexpr uint8_t kProductSchemaV1 = 1U;
constexpr uint8_t kPeriodicPayloadSizeV1 = 64U;
constexpr uint8_t kEventPayloadSizeV1 = 48U;
constexpr uint8_t kResultPayloadSizeV1 = 32U;
constexpr uint32_t kDataRecordsPerPage =
    (kPageSize - kPageHeaderSize) / kDataRecordSize;

constexpr uint32_t kControlRecordSize = 144U;
constexpr uint32_t kControlPayloadSize = 116U;
constexpr uint32_t kControlRecordsPerPage =
    (kPageSize - kPageHeaderSize) / kControlRecordSize;

constexpr uint32_t kPageStaticCrcOffset = 44U;
constexpr uint32_t kPageHeaderCommitOffset = 48U;
constexpr uint32_t kPageHeaderActiveOffset = 52U;
// Control-only one-way compaction journal: intent before target mutation,
// retirement after new authority has activated. Data headers stay unchanged.
constexpr uint32_t kControlIntentOffset = 56U;
constexpr uint32_t kControlIntentRetiredOffset = 60U;
constexpr uint32_t kControlIntent = 0x4F424349U;  // OBCI


constexpr uint32_t kDataRecordCrcOffset = 80U;
constexpr uint32_t kDataRecordCommitOffset = 84U;
constexpr uint32_t kDataRecordRelease0Offset = 88U;
constexpr uint32_t kDataRecordRelease1Offset = 92U;
constexpr uint32_t kDataRecordReleaseSlots = 2U;

constexpr uint32_t kControlRecordCrcOffset = 128U;
constexpr uint32_t kControlRecordCommitOffset = 132U;
constexpr uint32_t kControlRecordClear0Offset = 136U;
constexpr uint32_t kControlRecordClear1Offset = 140U;
constexpr uint32_t kControlRecordClearSlots = 2U;

constexpr uint32_t kCommit = 0U;
constexpr uint32_t kActive = 0U;
constexpr uint32_t kReleased = 0U;
constexpr uint32_t kCleared = 0U;

// Initial bounded SF5C control ownership. Two control pages are reserved by the
// portable format; SF5D chooses the physical address range later.
constexpr uint16_t kControlPageCount = 2U;
constexpr uint16_t kMaxExactCustodyObjects = 4U;
constexpr uint16_t kMaxOpenOccurrences = 8U;
constexpr uint16_t kMaxResultGuards = 8U;
constexpr uint16_t kStoreStateControls = 1U;
constexpr uint16_t kMaxActiveControls =
    kMaxExactCustodyObjects + kMaxOpenOccurrences + kMaxResultGuards +
    kStoreStateControls;

static_assert(kDataRecordsPerPage == 42U, "SF5C data page geometry changed");
static_assert(kPageHeaderSize + kDataRecordsPerPage * kDataRecordSize ==
                  kPageSize,
              "SF5C data page must be exact");
static_assert(kControlRecordsPerPage == 28U,
              "SF5C control page geometry changed");
static_assert(kPageHeaderSize + kControlRecordsPerPage * kControlRecordSize ==
                  kPageSize,
              "SF5C control page must be exact");
static_assert(kMaxActiveControls < kControlRecordsPerPage,
              "SF5C control compaction needs spare slots");

enum class PageKind : uint8_t {
  kData = 1U,
  kControl = 2U,
};

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
  PageKind kind = PageKind::kData;
  uint64_t generation = 0;
  uint64_t device_id = 0;
  uint64_t incarnation = 0;
};

struct RecordIdentity {
  uint64_t incarnation = 0;
  uint32_t sequence = 0;

  bool valid() const { return incarnation != 0U && sequence != 0U; }
};

enum class RecordKind : uint8_t {
  kPeriodic = 1U,
  kEvent = 2U,
  kResult = 3U,
};

enum class RecordEvidence : uint8_t {
  kErased,
  kRetained,
  kReleased,
  kStaged,
  kPartialCommit,
  kCorrupt,
};

struct RecordInspection {
  RecordEvidence evidence = RecordEvidence::kCorrupt;
  bool release_uncertain = false;
  uint8_t next_release_slot = UINT8_MAX;
  RecordKind kind = RecordKind::kPeriodic;
  uint8_t schema = 0;
  uint8_t payload_size = 0;
  uint64_t incarnation = 0;
  uint32_t sequence = 0;
  uint8_t payload[kDataPayloadSize]{};
};

enum class ControlKind : uint8_t {
  kExactObject = 1U,
  kOpenOccurrence = 2U,
  kResultGuard = 3U,
  kStoreState = 4U,
};

enum class ControlEvidence : uint8_t {
  kErased,
  kActive,
  kCleared,
  kStaged,
  kPartialCommit,
  kCorrupt,
};

struct ControlInspection {
  ControlEvidence evidence = ControlEvidence::kCorrupt;
  bool clear_uncertain = false;
  uint8_t next_clear_slot = UINT8_MAX;
  ControlKind kind = ControlKind::kExactObject;
  uint8_t schema = 0;
  uint16_t payload_size = 0;
  uint64_t serial = 0;
  uint8_t payload[kControlPayloadSize]{};
};

uint32_t crc32(const uint8_t* data, size_t size);
void put16(uint8_t* data, uint16_t value);
void put32(uint8_t* data, uint32_t value);
void put64(uint8_t* data, uint64_t value);
uint16_t get16(const uint8_t* data);
uint32_t get32(const uint8_t* data);
uint64_t get64(const uint8_t* data);
bool erased(const uint8_t* data, size_t size);

void encodePageHeader(PageKind kind, uint64_t generation, uint64_t device_id,
                      uint64_t incarnation, uint8_t* bytes);
bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection);

bool encodeRecord(RecordKind kind, uint8_t schema, uint64_t incarnation,
                  uint32_t sequence, const uint8_t* payload,
                  size_t payload_size, uint8_t* bytes);
bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection);

bool encodeControl(ControlKind kind, uint8_t schema, uint64_t serial,
                   const uint8_t* payload, size_t payload_size,
                   uint8_t* bytes);
bool inspectControl(const uint8_t* bytes, size_t size,
                    ControlInspection& inspection);

}  // namespace observation_store_format
}  // namespace orun_tlp
