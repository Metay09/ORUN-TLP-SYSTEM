#pragma once
#include <stdint.h>

namespace orun_tlp::storage_config {
// This is the core InternalFS partition, exclusively ORUN-owned while this
// backend is linked. It is not spare application flash.
constexpr uint32_t kBaseAddress = 0xED000;
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kPageCount = 7;
constexpr uint32_t kRegionSize = kPageSize * kPageCount;
constexpr uint32_t kPageHeaderSize = 352;
constexpr uint32_t kRecordSize = 36;
constexpr uint32_t kRecordsPerPage = (kPageSize - kPageHeaderSize) / kRecordSize;
constexpr uint32_t kCapacity = kPageCount * kRecordsPerPage;
constexpr uint64_t kSequenceBlockSize = 256;
constexpr uint32_t kSequenceSlotsPerPage = 8;
constexpr uint32_t kStateSlotsPerPage = 4;
static_assert(kBaseAddress + kRegionSize == 0xF4000, "bootloader boundary");
static_assert(kPageHeaderSize + kRecordsPerPage * kRecordSize == kPageSize, "page packing");

// M7P1 (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md) decided this future
// partition policy layout, carved from currently-unused application flash,
// strictly below the unchanged history region above. M7P2 adds these as
// inert layout constants and a build-time application ceiling guard only:
// there is no runtime owner, backend, or format for any of these regions
// yet. Do not treat these as an active partition until a later slice
// (M7P4/M7P5/M7P6 per the ADR) implements one.
constexpr uint32_t kFutureSecurityRegionPages = 2;
constexpr uint32_t kFutureConfigRegionPages = 2;
constexpr uint32_t kFutureBondRegionPages = 2;

constexpr uint32_t kFutureSecurityRegionStart = 0x0E7000;
constexpr uint32_t kFutureSecurityRegionEnd =
    kFutureSecurityRegionStart + kFutureSecurityRegionPages * kPageSize;
constexpr uint32_t kFutureConfigRegionStart = kFutureSecurityRegionEnd;
constexpr uint32_t kFutureConfigRegionEnd =
    kFutureConfigRegionStart + kFutureConfigRegionPages * kPageSize;
constexpr uint32_t kFutureBondRegionStart = kFutureConfigRegionEnd;
constexpr uint32_t kFutureBondRegionEnd =
    kFutureBondRegionStart + kFutureBondRegionPages * kPageSize;

// The M7P2 build guard (firmware/scripts/check_storage_layout.py) enforces
// that no linked application byte reaches this address; it derives its own
// ceiling literal from this exact symbol's value at build time (parsed from
// this file), so there is one source of truth, not two.
constexpr uint32_t kApplicationPolicyEndAddress = kFutureSecurityRegionStart;

static_assert(kFutureSecurityRegionStart % kPageSize == 0,
              "security region must start page-aligned");
static_assert(kFutureSecurityRegionEnd % kPageSize == 0,
              "security region must end page-aligned");
static_assert(kFutureConfigRegionEnd % kPageSize == 0,
              "config region must end page-aligned");
static_assert(kFutureBondRegionEnd % kPageSize == 0,
              "bond region must end page-aligned");

static_assert(kFutureSecurityRegionEnd - kFutureSecurityRegionStart ==
                  kFutureSecurityRegionPages * kPageSize,
              "security region must be exactly its declared page count");
static_assert(kFutureConfigRegionEnd - kFutureConfigRegionStart ==
                  kFutureConfigRegionPages * kPageSize,
              "config region must be exactly its declared page count");
static_assert(kFutureBondRegionEnd - kFutureBondRegionStart ==
                  kFutureBondRegionPages * kPageSize,
              "bond region must be exactly its declared page count");

// Chained equalities make the four regions exactly contiguous and
// non-overlapping by construction: application | security | config | bond |
// history, with no gap and no overlap anywhere in that chain.
static_assert(kApplicationPolicyEndAddress == kFutureSecurityRegionStart,
              "application policy ceiling must equal security region start");
static_assert(kFutureSecurityRegionEnd == kFutureConfigRegionStart,
              "security region must directly abut config region");
static_assert(kFutureConfigRegionEnd == kFutureBondRegionStart,
              "config region must directly abut bond region");
static_assert(kFutureBondRegionEnd == kBaseAddress,
              "bond region must end exactly at the unchanged history start");

// The existing history invariant above (kBaseAddress + kRegionSize ==
// 0xF4000) already proves the bootloader boundary is unchanged; restated
// here only as an explicit cross-check tying it to this new layout.
static_assert(kBaseAddress == 0x0ED000,
              "history start must remain unchanged by the M7P2 layout");
}  // namespace orun_tlp::storage_config
