#include "flash_backend.h"
#include "storage_config.h"

#include <Arduino.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <string.h>

extern uint32_t __flash_arduino_end[];

namespace {

bool synchronousFlashAvailable() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

}  // namespace

namespace orun_tlp {
using namespace storage_config;
namespace {

constexpr uint32_t kProductRegionSize = kProductRegionEnd - kProductRegionStart;
// Largest single program of either product store: an ObservationStore
// control record's body+CRC is 132 bytes (CustodyStore's largest is 80). Allow
// one whole 144-byte control slot without depending on the store formats.
constexpr size_t kMaxProgramSize = 144;

bool inBounds(uint32_t offset, size_t size) {
  return offset <= kProductRegionSize && size <= kProductRegionSize - offset;
}

}  // namespace

bool NrfProductFlash::begin() {
  const uint32_t boot = NRF_UICR->NRFFW[0];
  ready_ =
      reinterpret_cast<uintptr_t>(__flash_arduino_end) == kBaseAddress &&
      NRF_FICR->CODEPAGESIZE == kPageSize && NRF_FICR->CODESIZE == 256 &&
      (boot == UINT32_MAX ||
       boot >= kBaseAddress + storage_config::kRegionSize) &&
      synchronousFlashAvailable();
  return ready_;
}

bool NrfProductFlash::read(uint32_t offset, void* data, size_t size) const {
  if (!ready_ || data == nullptr || !inBounds(offset, size)) return false;
  memcpy(data, reinterpret_cast<const void*>(kProductRegionStart + offset),
         size);
  return true;
}

FlashOpResult NrfProductFlash::program(uint32_t offset, const void* data,
                                       size_t size) {
  if (!ready_ || data == nullptr || size == 0 || !inBounds(offset, size) ||
      (offset & 3U) != 0 || (size & 3U) != 0 || size > kMaxProgramSize ||
      size > kPageSize - offset % kPageSize)
    return FlashOpResult::kFailed;
  if (!synchronousFlashAvailable()) return FlashOpResult::kFailed;

  // NOR flash can only clear bits: never reprogram a byte without an erase.
  const auto* destination =
      reinterpret_cast<const uint8_t*>(kProductRegionStart + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;

  alignas(4) uint32_t words[kMaxProgramSize / sizeof(uint32_t)];
  memcpy(words, data, size);
  if (sd_flash_write(reinterpret_cast<uint32_t*>(kProductRegionStart + offset),
                     words, size / sizeof(uint32_t)) != NRF_SUCCESS)
    return FlashOpResult::kFailed;

  return memcmp(destination, data, size) == 0 ? FlashOpResult::kDone
                                              : FlashOpResult::kFailed;
}

FlashOpResult NrfProductFlash::erasePage(uint32_t page) {
  if (!ready_ || page >= kProductRegionPages || !synchronousFlashAvailable())
    return FlashOpResult::kFailed;

  if (sd_flash_page_erase(kProductRegionStart / kPageSize + page) !=
      NRF_SUCCESS)
    return FlashOpResult::kFailed;

  const auto* words = reinterpret_cast<const uint32_t*>(
      kProductRegionStart + page * kPageSize);
  for (uint32_t index = 0; index < kPageSize / sizeof(uint32_t); ++index)
    if (words[index] != UINT32_MAX) return FlashOpResult::kFailed;
  return FlashOpResult::kDone;
}

}  // namespace orun_tlp
