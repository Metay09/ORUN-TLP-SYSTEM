#include "flash_backend.h"
#include "storage_config.h"
#include <Arduino.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <string.h>

extern uint32_t __flash_arduino_end[];

namespace {

// Mirrors NrfHistoryFlash/NrfConfigFlash's own recheck-before-every-mutation
// discipline. M0-M7P6B never enables SoftDevice on real hardware; M7P7 must
// design event routing before this path is exercised.
bool synchronousFlashAvailable() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

}  // namespace

namespace orun_tlp {
using namespace storage_config;
namespace {
constexpr uint32_t kSecurityRegionSize = kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
// Bounds the local staging buffer for program(); the largest blob
// SecurityStore ever writes is security_format::kCredentialRecordSize (68
// bytes today), far under one page -- 96 is a generous, still-small fixed
// cap so this file does not need to depend on security_format.h at all.
constexpr size_t kMaxProgramSize = 96;
bool inBounds(uint32_t offset, size_t size) {
  return offset <= kSecurityRegionSize && size <= kSecurityRegionSize - offset;
}
}  // namespace

bool NrfSecurityFlash::begin() {
  // `__flash_arduino_end` is the vendor linker script's own fixed FLASH
  // MEMORY region end -- always equal to kBaseAddress (history's start)
  // regardless of application size, exactly as NrfHistoryFlash::begin() and
  // NrfConfigFlash::begin() already check; reused verbatim here since it is
  // the same one linker fact underlying every partition below history.
  const uint32_t boot = NRF_UICR->NRFFW[0];
  ready_ = reinterpret_cast<uintptr_t>(__flash_arduino_end) == kBaseAddress &&
      NRF_FICR->CODEPAGESIZE == kPageSize && NRF_FICR->CODESIZE == 256 &&
      (boot == UINT32_MAX || boot >= kBaseAddress + storage_config::kRegionSize) &&
      synchronousFlashAvailable();
  return ready_;
}
bool NrfSecurityFlash::read(uint32_t offset, void* data, size_t size) const {
  if (!ready_ || data == nullptr || !inBounds(offset, size)) return false;
  memcpy(data, reinterpret_cast<const void*>(kFutureSecurityRegionStart + offset), size);
  return true;
}
FlashOpResult NrfSecurityFlash::program(uint32_t offset, const void* data, size_t size) {
  if (!ready_ || !data || !size || !inBounds(offset, size) ||
      (offset & 3U) != 0 || (size & 3U) != 0 || size > kMaxProgramSize ||
      size > kPageSize - offset % kPageSize) return FlashOpResult::kFailed;
  if (!synchronousFlashAvailable()) return FlashOpResult::kFailed;
  const auto* destination = reinterpret_cast<const uint8_t*>(kFutureSecurityRegionStart + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return FlashOpResult::kFailed;
  alignas(4) uint32_t words[kMaxProgramSize / sizeof(uint32_t)];
  memcpy(words, data, size);
  // Disabled SoftDevice: NRF_SUCCESS means physically completed, no event.
  if (sd_flash_write(reinterpret_cast<uint32_t*>(kFutureSecurityRegionStart + offset), words,
                     size / sizeof(uint32_t)) != NRF_SUCCESS) return FlashOpResult::kFailed;
  return memcmp(destination, data, size) == 0 ? FlashOpResult::kDone : FlashOpResult::kFailed;
}
FlashOpResult NrfSecurityFlash::erasePage(uint32_t page) {
  if (!ready_ || page >= kFutureSecurityRegionPages || !synchronousFlashAvailable())
    return FlashOpResult::kFailed;
  if (sd_flash_page_erase(kFutureSecurityRegionStart / kPageSize + page) != NRF_SUCCESS)
    return FlashOpResult::kFailed;
  const auto* words = reinterpret_cast<const uint32_t*>(
      kFutureSecurityRegionStart + page * kPageSize);
  for (uint32_t index = 0; index < kPageSize / sizeof(uint32_t); ++index)
    if (words[index] != UINT32_MAX) return FlashOpResult::kFailed;
  return FlashOpResult::kDone;
}
}  // namespace orun_tlp
