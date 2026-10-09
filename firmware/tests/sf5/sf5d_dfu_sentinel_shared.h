// SF5D TEST-ONLY DFU retention sentinel. No production storage ownership.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <nrf.h>
#include <string.h>

#include "storage_config.h"

namespace sf5d_dfu_sentinel {

// Observe three pages in the unallocated candidate region. Existing nonblank
// content in the first two pages is NEVER modified; only the third page may
// receive a sentinel, and only after explicit user confirmation.
constexpr uint32_t kAddresses[] = {0x0C5000U, 0x0D5000U, 0x0E4000U};
constexpr uint32_t kPageSize = orun_tlp::storage_config::kPageSize;
constexpr uint32_t kMagic = 0x53463544U;  // "SF5D"
constexpr uint32_t kTrial = 0x20261009U;
constexpr uint32_t kThirdWord = 0xA4F21C39U;
constexpr unsigned kSampleCount = sizeof(kAddresses) / sizeof(kAddresses[0]);
constexpr uint32_t kSeedAddress = 0x0E4000U;

static_assert(kPageSize == 4096U, "expected nRF52840 page size");
static_assert(kAddresses[0] == 0x0C5000U, "candidate lower bound moved");
static_assert(kAddresses[1] > kAddresses[0] + kPageSize, "overlapping markers");
static_assert(kAddresses[2] + kPageSize ==
                  orun_tlp::storage_config::kGeofenceRegionStart,
              "sample must stop before the owned geofence region");
static_assert(kAddresses[0] >= 0x0C5000U, "sample below candidate");
static_assert(kSeedAddress == kAddresses[2], "seed must only target last sample");

inline void expectedWords(uint32_t address, uint32_t (&words)[4]) {
  words[0] = kMagic;
  words[1] = address;
  words[2] = kThirdWord ^ kTrial;
  words[3] = ~(words[0] ^ words[1] ^ words[2]);
}

inline bool markerMatches(uint32_t address) {
  uint32_t expected[4]{};
  expectedWords(address, expected);
  const auto* stored = reinterpret_cast<const volatile uint32_t*>(address);
  for (unsigned i = 0; i < 4; ++i)
    if (stored[i] != expected[i]) return false;
  return true;
}

inline bool pageErased(uint32_t address) {
  const auto* stored = reinterpret_cast<const volatile uint32_t*>(address);
  for (unsigned i = 0; i < kPageSize / sizeof(uint32_t); ++i)
    if (stored[i] != UINT32_MAX) return false;
  return true;
}

// Each page fingerprint covers all 4096 bytes. The existing OTHER pages
// provide a read-only control sample across the subsequent DFU upload.
inline uint32_t pageCrc32(uint32_t address) {
  const auto* bytes = reinterpret_cast<const volatile uint8_t*>(address);
  uint32_t crc = UINT32_MAX;
  for (uint32_t index = 0; index < kPageSize; ++index) {
    crc ^= static_cast<uint32_t>(bytes[index]);
    for (unsigned bit = 0; bit < 8U; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return ~crc;
}

inline void printStatus() {
  unsigned matched = 0;
  unsigned erased = 0;
  for (unsigned i = 0; i < kSampleCount; ++i) {
    const uint32_t address = kAddresses[i];
    const bool match = markerMatches(address);
    const bool empty = !match && pageErased(address);
    matched += match ? 1U : 0U;
    erased += empty ? 1U : 0U;
    const auto* stored = reinterpret_cast<const volatile uint32_t*>(address);
    Serial.printf(
        "SF5D DFU PAGE address=0x%06lX state=%s first=0x%08lX "
        "crc32=0x%08lX\n",
        static_cast<unsigned long>(address),
        match ? "MATCH" : (empty ? "ERASED" : "OTHER"),
        static_cast<unsigned long>(stored[0]),
        static_cast<unsigned long>(pageCrc32(address)));
  }
  Serial.printf(
      "SF5D DFU STATUS matched=%u/%u erased=%u/%u "
      "seed_address=0x%06lX seed_state=%s\n",
      matched, kSampleCount, erased, kSampleCount,
      static_cast<unsigned long>(kSeedAddress),
      markerMatches(kSeedAddress)
          ? "MATCH"
          : (pageErased(kSeedAddress) ? "ERASED" : "OTHER"));
}

inline void feedInheritedWatchdog() {
  if (NRF_WDT->RUNSTATUS != 0)
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

inline bool acceptLine(const char* line, const char* expected) {
  return strcmp(line, expected) == 0;
}

}  // namespace sf5d_dfu_sentinel
