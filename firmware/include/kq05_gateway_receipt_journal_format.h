#pragma once
#include <stddef.h>
#include <stdint.h>

#include "storage_config.h"

namespace orun_tlp {
namespace kq05_receipt_journal {

// KQ05 PC-only format candidate. NOT mounted, no writer, no erase, no ACK.
// The 28 KiB legacy History range is NOT physically repurposed by this header.
constexpr uint32_t kHistoryBase = storage_config::kBaseAddress;
constexpr uint32_t kHistoryBytes = storage_config::kRegionSize;
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kReceiptPageCount = 2U;
constexpr uint32_t kReservedPageCount =
    storage_config::kPageCount - kReceiptPageCount;
constexpr uint32_t kReceiptStart = kHistoryBase;
constexpr uint32_t kReceiptEnd =
    kReceiptStart + kReceiptPageCount * kPageSize;
constexpr uint32_t kReservedStart = kReceiptEnd;
constexpr uint32_t kReservedEnd = kHistoryBase + kHistoryBytes;

constexpr size_t kPageHeaderSize = 64U;
constexpr size_t kRecordSize = 144U;
constexpr size_t kOfferMax = 124U;
constexpr size_t kOfferMin = 96U;
constexpr size_t kRecordsPerPage =
    (kPageSize - kPageHeaderSize) / kRecordSize;  // 28, not 31
constexpr size_t kCapacity = kReceiptPageCount * kRecordsPerPage; // 56

static_assert(kReceiptPageCount < storage_config::kPageCount,
              "receipt plan must leave expansion pages untouched");
static_assert(kReceiptStart == 0xED000U &&
                  kReceiptEnd == 0xEF000U &&
                  kReservedEnd == 0xF4000U,
              "KQ05 candidate history geometry changed");
static_assert(kPageHeaderSize + kRecordsPerPage * kRecordSize == kPageSize,
              "KQ05 fixed records must fit exactly in one NOR page");
static_assert(kReceiptEnd <= storage_config::kBaseAddress +
                 storage_config::kRegionSize,
              "KQ05 proposal exceeds legacy region");

// A valid record is structural evidence ONLY. Backend receipt authenticity,
// Edge/Gateway HMAC, Gateway enrollment and custody are NOT granted here.
enum class Evidence : uint8_t { kErased, kIncomplete, kCommitted, kCorrupt };
struct HeaderView {
  uint64_t gateway_id = 0U;
  uint64_t generation = 0U;
};

uint32_t crc32(const uint8_t* bytes, size_t size);
bool encodeHeader(uint64_t gateway_id, uint64_t generation,
                  uint8_t (&out)[kPageHeaderSize]);
Evidence inspectHeader(const uint8_t* bytes, size_t size, HeaderView& view);
// Program the first 60 bytes, read back, then program ONLY [60..64) as
// the commit marker. The caller must verify the entire committed header.
void headerCommitMarker(uint8_t (&out)[4]);

bool encodeRecord(const uint8_t* logical_offer, size_t size,
                  uint8_t (&out)[kRecordSize]);
Evidence inspectRecord(const uint8_t* bytes, size_t size,
                       uint8_t (&logical_offer)[kOfferMax],
                       size_t& offer_size);
// Program [0..140), read back, then program ONLY [140..144).
// The caller must verify the complete committed record before STAGED.
// A marker is NOT proof of a successful Flash mutation on its own.
void recordCommitMarker(uint8_t (&out)[4]);

}  // namespace kq05_receipt_journal
}  // namespace orun_tlp
