#pragma once

// SF5G distinct page-header / reclaim-intent CANDIDATE.
// Host-only. This is a parser/encoder, NOT a FlashBackend, a mount policy,
// a completed reclaim protocol, a durable write proof, or ACK authorization.
#include <stddef.h>
#include <stdint.h>

#include "sf5g_custody_record_candidate.h"

namespace orun_tlp {
namespace sf5g_page_candidate {

namespace record = sf5g_custody_candidate;

constexpr uint32_t kMagic = 0x4F435033U;  // "OCP3"; not OCQ1 or OBS1
constexpr uint16_t kVersion = 3U;
constexpr size_t kPageSize = record::kPageSize;
constexpr size_t kHeaderSize = record::kPageHeaderSize;
constexpr size_t kRecordsPerPage = record::kRecordsPerPage;
constexpr size_t kRecordSize = record::kRecordSize;
constexpr size_t kObjectMax = record::kObjectMax;
constexpr size_t kIntentSize = record::kIntentSize;
constexpr size_t kIntentsPerPage = record::kIntentsPerPage;
// The 128 KiB product flash region has at most 32 physical 4 KiB pages.
// A future runtime must ALSO check its actual page count, ownership,
// source != target, generation and live custody before any erase.
constexpr uint16_t kMaxRegionPages = 32U;
static_assert(kMaxRegionPages * kPageSize == 128U * 1024U,
             "product region size changed");

constexpr size_t kStaticCrcOffset = 28U;
constexpr size_t kCommitOffset = 32U;
constexpr size_t kActiveOffset = 36U;
constexpr size_t kReservedOffset = 40U;

constexpr size_t kIntentCrcOffset = 12U;
constexpr size_t kIntentCommitOffset = 16U;
constexpr size_t kIntentDoneOffset = 20U;
constexpr uint32_t kCommitted = 0U;
constexpr uint32_t kErased = 0xFFFFFFFFU;

static_assert(kReservedOffset <= kHeaderSize, "header must fit");
static_assert(kStaticCrcOffset + 4U == kCommitOffset, "header CRC layout");
static_assert(kActiveOffset + 4U == kReservedOffset, "header markers");
static_assert(kIntentDoneOffset + 4U == kIntentSize, "intent must fit");
static_assert(kHeaderSize + kRecordsPerPage * kRecordSize +
                  record::kSeparationGap + kIntentsPerPage * kIntentSize ==
              kPageSize, "candidate page geometry");

enum class HeaderEvidence : uint8_t {
  kErased,
  kStaged,
  kPartialCommit,
  kPrepared,
  kPartialActivation,
  kActive,
  kCorrupt,
  kForeign,
};

struct HeaderInspection {
  HeaderEvidence evidence = HeaderEvidence::kCorrupt;
  // Diagnostics only on partial activation: never ownership/erase/ACK proof.
  uint64_t generation = 0U;
};

// All bytes after offset 40 are erased; commit and activation words stay
// erased. Writer must program/verify body, commit then verify, activate and
// verify. Neither function touches actual flash.
bool encodeStagedHeader(uint64_t generation, uint8_t* out, size_t out_size);
bool inspectHeader(const uint8_t* bytes, size_t size, HeaderInspection& out);

enum class IntentEvidence : uint8_t {
  kErased,
  kStaged,
  kPartialCommit,
  kCommitted,
  kPartialCompletion,
  kCompleted,
  kCorrupt,
};

struct IntentInspection {
  IntentEvidence evidence = IntentEvidence::kCorrupt;
  // CRC-validated fields may be exposed on partial completion solely for
  // diagnosis/reconciliation; no erase, completion or ACK authority.
  uint16_t target_page = UINT16_MAX;
  uint64_t target_generation = 0U;
};

// Intended for a future bounded reclaim state machine. This codec cannot
// authorize any erase or prove that the target page has been reclaimed.
bool encodeStagedIntent(uint16_t target_page, uint64_t target_generation,
                        uint8_t* out, size_t out_size);
bool inspectIntent(const uint8_t* bytes, size_t size, IntentInspection& out);

}  // namespace sf5g_page_candidate
}  // namespace orun_tlp
