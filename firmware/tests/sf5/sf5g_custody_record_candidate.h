#pragma once

// SF5G CANDIDATE RECORD CODEC. Host-only, NOT an approved on-flash format.
// Does not decide custody ACK eligibility, authorization or flash ownership.
#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace sf5g_custody_candidate {

constexpr size_t kRecordSize = 120U;
constexpr size_t kObjectMax = 100U;
constexpr size_t kRecordLengthOffset = 0U;
constexpr size_t kRecordObjectOffset = 4U;
constexpr size_t kRecordCrcOffset = 104U;
constexpr size_t kRecordCommitOffset = 108U;
constexpr size_t kRecordHandoff0Offset = 112U;
constexpr size_t kRecordHandoff1Offset = 116U;
constexpr uint32_t kProgrammedMarker = 0U;
constexpr uint32_t kErasedMarker = 0xFFFFFFFFU;

constexpr size_t kPageSize = 4096U;
constexpr size_t kPageHeaderSize = 64U;
constexpr size_t kRecordsPerPage = 32U;
constexpr size_t kIntentSize = 24U;
constexpr size_t kIntentsPerPage = 3U;
constexpr size_t kSeparationGap =
    kPageSize - kPageHeaderSize - kRecordsPerPage * kRecordSize -
    kIntentsPerPage * kIntentSize;
static_assert(kRecordHandoff1Offset + 4U == kRecordSize,
              "SF5G candidate record geometry");
static_assert(kSeparationGap == 120U, "SF5G candidate page geometry");

// Structural checks do NOT authenticate AEAD or establish a trusted origin.
bool productObjectShape(const uint8_t* object, size_t size);
uint32_t crc32(const uint8_t* bytes, size_t size);

// Prepare a complete data/CRC blob but KEEP commit and handoff markers erased.
// A future flash writer must write, read-verify, then separately commit.
// On rejected input, output is unchanged.
bool encodeStagedRecord(const uint8_t* object, size_t size,
                        uint8_t* output, size_t output_size);

enum class Evidence : uint8_t {
  kErased,
  kStaged,
  kPartialCommit,
  kCorrupt,
  kHeld,
  kHandedOff,
};

struct RecordInspection {
  Evidence evidence = Evidence::kCorrupt;
  uint16_t object_size = 0U;
  uint8_t object[kObjectMax]{};
  bool handoff_uncertain = false;
  uint8_t next_handoff_slot = 0xFFU;
};

// Reader never treats a staged or corrupt record as durable. No ACK API.
bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& result);
bool sameExactObject(const RecordInspection& result,
                     const uint8_t* object, size_t size);

}  // namespace sf5g_custody_candidate
}  // namespace orun_tlp
