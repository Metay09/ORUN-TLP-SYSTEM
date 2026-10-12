// SF5D4 TEST-ONLY full product-region DFU retention, [0x0C5000, 0x0E5000).
// SF5D5 reserved this region for the product store; the probe images stay
// test-only and must only run on a development device whose region holds no
// store data (seeding leaves marker pages that a store mount will refuse).
//
// SF5D3 sampled 3 of the 32 candidate pages across one small-image DFU. This
// covers all 32 pages across an update with the full production image: the
// seed image marks every fully erased page (16 bytes at its start, never an
// erase), so after seeding no page is blank and an erase by a later update
// shows up as a changed page CRC. Pages that already hold other data are left
// as they are and serve as read-only controls.
#pragma once
#include "sf5d_dfu_sentinel_shared.h"

namespace sf5d_full_region {

using namespace sf5d_dfu_sentinel;

constexpr uint32_t kStart = orun_tlp::storage_config::kProductRegionStart;
constexpr uint32_t kEnd = orun_tlp::storage_config::kProductRegionEnd;
constexpr unsigned kPages = (kEnd - kStart) / kPageSize;

static_assert(kStart == 0x0C5000U, "probe covers the reserved product region");
static_assert(kEnd == orun_tlp::storage_config::kGeofenceRegionStart,
              "product region must stop at the geofence region");
static_assert(kPages == 32U, "product region is 128 KiB = 32 pages");

inline uint32_t pageAddress(unsigned index) { return kStart + index * kPageSize; }

// Digest of all 32 page CRCs, in address order, for a one-line comparison.
inline uint32_t digestUpdate(uint32_t crc, uint32_t value) {
  for (unsigned byte = 0; byte < 4U; ++byte) {
    crc ^= (value >> (8U * byte)) & 0xFFU;
    for (unsigned bit = 0; bit < 8U; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc;
}

inline void printFullStatus() {
  unsigned matched = 0, erased = 0, other = 0;
  uint32_t digest = UINT32_MAX;
  for (unsigned i = 0; i < kPages; ++i) {
    feedInheritedWatchdog();
    const uint32_t address = pageAddress(i);
    const bool match = markerMatches(address);
    const bool empty = !match && pageErased(address);
    matched += match ? 1U : 0U;
    erased += empty ? 1U : 0U;
    other += (!match && !empty) ? 1U : 0U;
    const uint32_t crc = pageCrc32(address);
    digest = digestUpdate(digest, crc);
    Serial.printf("SF5D FULL PAGE address=0x%06lX state=%s crc32=0x%08lX\n",
                  static_cast<unsigned long>(address),
                  match ? "MATCH" : (empty ? "ERASED" : "OTHER"),
                  static_cast<unsigned long>(crc));
  }
  Serial.printf("SF5D FULL STATUS pages=%u matched=%u erased=%u other=%u "
                "digest=0x%08lX\n",
                kPages, matched, erased, other,
                static_cast<unsigned long>(~digest));
}

}  // namespace sf5d_full_region
