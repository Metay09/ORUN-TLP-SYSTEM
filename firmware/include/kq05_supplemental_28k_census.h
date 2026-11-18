#pragma once

#include <stdint.h>

#include "custody_store_product_format.h"
#include "flash_backend.h"
#include "observation_store_format.h"
#include "storage_config.h"

namespace orun_tlp {
namespace kq05_supplemental {

// PC-only proof for the proposed seven-page supplementary area. This is a
// READ-ONLY census, not a store, migration, formatter or mount authority.
// The FlashBackend supplied here must already map exactly the OLD 28 KiB
// history interval. It is never used to program or erase any byte.
constexpr uint32_t kStart = storage_config::kBaseAddress;
constexpr uint32_t kPages = storage_config::kPageCount;
constexpr uint32_t kPageBytes = storage_config::kPageSize;
constexpr uint32_t kBytes = storage_config::kRegionSize;
constexpr uint32_t kEnd = kStart + kBytes;

constexpr uint32_t kTrackerNominalEntries =
    (kPages - observation_store_format::kControlPageCount) *
    observation_store_format::kDataRecordsPerPage;  // 210
constexpr uint32_t kGatewayNominalEntries =
    kPages * custody_store_product_format::kRecordsPerPage;  // 231

static_assert(kStart == 0xED000U && kPages == 7U &&
              kEnd == 0xF4000U && kBytes == 28U * 1024U,
              "legacy 28 KiB geometry must remain fixed");
static_assert(kPageBytes == observation_store_format::kPageSize &&
              kPageBytes == custody_store_product_format::kPageSize,
              "product format page sizes must match physical history Flash");
static_assert(kTrackerNominalEntries == 210U &&
              kGatewayNominalEntries == 231U,
              "nominal capacity arithmetic changed");

// Role MUST come from local authoritative configuration, never from
// Bluetooth/LoRa bytes or a caller's untrusted claim.
enum class Role : uint8_t {
  kUnassigned, kTracker, kGateway, kConflict
};

enum class State : uint8_t {
  kBlockedRole,
  kReadFailed,
  kBlank,               // all 28 KiB are physically erased, no mount implied
  kPotentialTracker,    // OBS1 headers structurally valid, store scan pending
  kGatewayNeedsBinding, // OCQ2/v3 unbound; NOT mountable in 28 KiB
  kGatewayBoundCandidate, // v4 bound to local Gateway, full recovery pending
  kLegacyOrForeign,     // unknown format, wrong role/device, partial page
  kCorruptOrIncomplete  // torn or malformed header; no automatic repair
};

struct Result {
  State state = State::kReadFailed;
  uint8_t erased_pages = 0U;
  uint8_t candidate_pages = 0U;
  uint8_t first_bad_page = UINT8_MAX;
};

// Checks all page bytes for erasure before classifying a page as blank.
// Never initializes Flash, writes a header, erases old History, mints ACK,
// assumes that a structurally valid page means trusted contents, or opens
// either the existing 128 KiB store or this supplemental region.
// A kPotentialTracker result is NOT authorization to mount ObservationStore.
// kGatewayNeedsBinding MUST NOT permit a 39-page ProductCustodyStore mount.
// Only v4, device-bound OCQ2 is a candidate; full durable store recovery
// remains a separate, mandatory operation. No result authorizes ACK minting.
Result inspectReadOnly(const FlashBackend& history_port, Role local_role,
                       uint64_t local_device_id);

}  // namespace kq05_supplemental
}  // namespace orun_tlp
