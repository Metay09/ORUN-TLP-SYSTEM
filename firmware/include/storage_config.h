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
// strictly below the unchanged history region above. M7P2 added these as
// inert layout constants and a build-time application ceiling guard.
// M7P4 gave the bond region a real owner (relocated InternalFS). M7P5 gave
// the config region a real owner (ConfigStore, firmware/include/config_store.h).
// M7P6B gave the security region a real owner (SecurityStore,
// firmware/include/security_store.h) -- durable credential/TX-nonce
// persistence only; no cryptography, secure RF envelope or BLE runtime.
// M6D3A reserves a two-page GeofenceStore region immediately below the
// existing M7 chain. This slice reserves/protects the bytes only; it does not
// add a GeofenceStore writer or runtime owner.
// The "kFuture*" names are kept even after a region gains an owner, matching
// M7P4's own precedent, to avoid unrelated renames/churn.
constexpr uint32_t kGeofenceRegionPages = 2;
constexpr uint32_t kFutureSecurityRegionPages = 2;
constexpr uint32_t kFutureConfigRegionPages = 2;
constexpr uint32_t kFutureBondRegionPages = 2;

constexpr uint32_t kGeofenceRegionStart = 0x0E5000;
constexpr uint32_t kGeofenceRegionEnd =
    kGeofenceRegionStart + kGeofenceRegionPages * kPageSize;

constexpr uint32_t kFutureSecurityRegionStart = 0x0E7000;
constexpr uint32_t kFutureSecurityRegionEnd =
    kFutureSecurityRegionStart + kFutureSecurityRegionPages * kPageSize;
constexpr uint32_t kFutureConfigRegionStart = kFutureSecurityRegionEnd;
constexpr uint32_t kFutureConfigRegionEnd =
    kFutureConfigRegionStart + kFutureConfigRegionPages * kPageSize;
constexpr uint32_t kFutureBondRegionStart = kFutureConfigRegionEnd;
constexpr uint32_t kFutureBondRegionEnd =
    kFutureBondRegionStart + kFutureBondRegionPages * kPageSize;

// The post-link guard in scripts/check_storage_layout.py enforces that no
// linked application byte reaches the lowest reserved data region. M6D3A moves
// that boundary from SecurityStore's 0x0E7000 down to GeofenceStore's 0x0E5000.
// The parser validates this alias plus the contiguous geofence->security chain.
constexpr uint32_t kApplicationPolicyEndAddress = kGeofenceRegionStart;

static_assert(kGeofenceRegionStart % kPageSize == 0,
              "geofence region must start page-aligned");
static_assert(kGeofenceRegionEnd % kPageSize == 0,
              "geofence region must end page-aligned");
static_assert(kFutureSecurityRegionStart % kPageSize == 0,
              "security region must start page-aligned");
static_assert(kFutureSecurityRegionEnd % kPageSize == 0,
              "security region must end page-aligned");
static_assert(kFutureConfigRegionEnd % kPageSize == 0,
              "config region must end page-aligned");
static_assert(kFutureBondRegionEnd % kPageSize == 0,
              "bond region must end page-aligned");

static_assert(kGeofenceRegionEnd - kGeofenceRegionStart ==
                  kGeofenceRegionPages * kPageSize,
              "geofence region must be exactly its declared page count");
static_assert(kFutureSecurityRegionEnd - kFutureSecurityRegionStart ==
                  kFutureSecurityRegionPages * kPageSize,
              "security region must be exactly its declared page count");
static_assert(kFutureConfigRegionEnd - kFutureConfigRegionStart ==
                  kFutureConfigRegionPages * kPageSize,
              "config region must be exactly its declared page count");
static_assert(kFutureBondRegionEnd - kFutureBondRegionStart ==
                  kFutureBondRegionPages * kPageSize,
              "bond region must be exactly its declared page count");

// Chained equalities make the reserved regions exactly contiguous and
// non-overlapping by construction: application | geofence | security | config
// | bond | history, with no gap and no overlap anywhere in that chain.
static_assert(kApplicationPolicyEndAddress == kGeofenceRegionStart,
              "application policy ceiling must equal geofence region start");
static_assert(kGeofenceRegionEnd == kFutureSecurityRegionStart,
              "geofence region must directly abut security region");
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
