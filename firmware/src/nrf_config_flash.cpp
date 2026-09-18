#include "flash_backend.h"
#include "storage_config.h"
#include <Arduino.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <string.h>

extern uint32_t __flash_arduino_end[];

namespace {

// Mirrors NrfHistoryFlash's own recheck-before-every-mutation discipline
// (nrf_history_flash.cpp). M0-M7P5 never enables SoftDevice on real
// hardware; M7P7 must design event routing before this path is exercised.
bool synchronousFlashAvailable() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

}  // namespace

namespace orun_tlp {
using namespace storage_config;
namespace {
constexpr uint32_t kConfigRegionSize = kFutureConfigRegionEnd - kFutureConfigRegionStart;
// Bounds the local staging buffer for program(); the largest blob
// ConfigStore ever writes is config_format::kRecordSize (36 bytes today),
// far under one page -- 64 is a generous, still-small fixed cap so this
// file does not need to depend on config_format.h at all.
constexpr size_t kMaxProgramSize = 64;
bool inBounds(uint32_t offset, size_t size) {
  return offset <= kConfigRegionSize && size <= kConfigRegionSize - offset;
}
}  // namespace

bool NrfConfigFlash::begin() {
  // `__flash_arduino_end` is the vendor linker script's own fixed FLASH
  // MEMORY region end (`ORIGIN=0x26000,LENGTH=0xED000-0x26000`, unchanged by
  // M7P1-M7P4's deliberate choice not to patch the linker -- M7P2 enforces
  // the application ceiling with a build-time guard instead, see
  // storage_config.h/check_storage_layout.py). It always equals history's
  // start (kBaseAddress) regardless of how much of the application region
  // is actually used, exactly as NrfHistoryFlash::begin() already checks --
  // reused verbatim here rather than duplicated with a different, incorrect
  // comparison, since it is the same one linker fact underlying every
  // partition below history, not something specific to history alone.
  const uint32_t boot = NRF_UICR->NRFFW[0];
  ready_ = reinterpret_cast<uintptr_t>(__flash_arduino_end) == kBaseAddress &&
      NRF_FICR->CODEPAGESIZE == kPageSize && NRF_FICR->CODESIZE == 256 &&
      (boot == UINT32_MAX || boot >= kBaseAddress + storage_config::kRegionSize) &&
      synchronousFlashAvailable();
  return ready_;
}
bool NrfConfigFlash::read(uint32_t offset, void* data, size_t size) const {
  if (!ready_ || data == nullptr || !inBounds(offset, size)) return false;
  memcpy(data, reinterpret_cast<const void*>(kFutureConfigRegionStart + offset), size);
  return true;
}
FlashOpResult NrfConfigFlash::program(uint32_t offset, const void* data, size_t size) {
  if (!ready_ || !data || !size || !inBounds(offset, size) ||
      (offset & 3U) != 0 || (size & 3U) != 0 || size > kMaxProgramSize ||
      size > kPageSize - offset % kPageSize) return FlashOpResult::kFailed;
  if (!synchronousFlashAvailable()) return FlashOpResult::kFailed;
  const auto* destination = reinterpret_cast<const uint8_t*>(kFutureConfigRegionStart + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;
  alignas(4) uint32_t words[kMaxProgramSize / sizeof(uint32_t)];
  memcpy(words, data, size);
  // Disabled SoftDevice: NRF_SUCCESS means physically completed, no event.
  if (sd_flash_write(reinterpret_cast<uint32_t*>(kFutureConfigRegionStart + offset), words,
                     size / sizeof(uint32_t)) != NRF_SUCCESS) return FlashOpResult::kFailed;
  return memcmp(destination, data, size) == 0 ? FlashOpResult::kDone : FlashOpResult::kFailed;
}
FlashOpResult NrfConfigFlash::erasePage(uint32_t page) {
  if (!ready_ || page >= kFutureConfigRegionPages || !synchronousFlashAvailable())
    return FlashOpResult::kFailed;
  if (sd_flash_page_erase(kFutureConfigRegionStart / kPageSize + page) != NRF_SUCCESS)
    return FlashOpResult::kFailed;
  const auto* words = reinterpret_cast<const uint32_t*>(
      kFutureConfigRegionStart + page * kPageSize);
  for (uint32_t index = 0; index < kPageSize / sizeof(uint32_t); ++index)
    if (words[index] != UINT32_MAX) return FlashOpResult::kFailed;
  return FlashOpResult::kDone;
}
}  // namespace orun_tlp
