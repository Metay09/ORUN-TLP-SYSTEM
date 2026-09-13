#include "flash_backend.h"
#include "storage_config.h"
#include <Arduino.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include <string.h>

extern uint32_t __flash_arduino_end[];

namespace {

// M0-M5 never enables SoftDevice. Recheck before EVERY mutating operation.
// M7 must design event routing and InternalFS ownership together before
// enabling asynchronous persistence. A state-query error also fails closed.
bool synchronousFlashAvailable() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

}  // namespace

namespace orun_tlp {
using namespace storage_config;
static bool inBounds(uint32_t offset, size_t size) { return offset <= kRegionSize && size <= kRegionSize-offset; }
bool NrfHistoryFlash::begin() {
  const uint32_t boot = NRF_UICR->NRFFW[0];
  ready_ = reinterpret_cast<uintptr_t>(__flash_arduino_end) == kBaseAddress &&
      NRF_FICR->CODEPAGESIZE == kPageSize && NRF_FICR->CODESIZE == 256 &&
      (boot == UINT32_MAX || boot >= kBaseAddress + kRegionSize) &&
      synchronousFlashAvailable();
  return ready_;
}
bool NrfHistoryFlash::read(uint32_t offset, void* data, size_t size) const {
  if (!ready_ || data == nullptr || !inBounds(offset, size)) return false;
  memcpy(data, reinterpret_cast<const void*>(kBaseAddress + offset), size);
  return true;
}
bool NrfHistoryFlash::program(uint32_t offset, const void* data, size_t size) {
  if(!ready_ || !data || !size || !inBounds(offset,size) ||
     (offset & 3U) != 0 || (size & 3U) != 0 || size > kPageHeaderSize ||
     size > kPageSize - offset % kPageSize) return false;
  if (!synchronousFlashAvailable()) return false;
  const auto* destination = reinterpret_cast<const uint8_t*>(kBaseAddress + offset);
  for (size_t index = 0; index < size; ++index)
    if (destination[index] != 0xFF) return false;
  alignas(4) uint32_t words[kPageHeaderSize / sizeof(uint32_t)];
  memcpy(words, data, size);
  // Disabled SoftDevice: NRF_SUCCESS means physically completed, no event.
  if (sd_flash_write(reinterpret_cast<uint32_t*>(kBaseAddress + offset), words,
                     size / sizeof(uint32_t)) != NRF_SUCCESS) return false;
  return memcmp(destination, data, size) == 0;
}
bool NrfHistoryFlash::erasePage(uint32_t page) {
  if (!ready_ || page >= kPageCount || !synchronousFlashAvailable()) return false;
  if (sd_flash_page_erase(kBaseAddress / kPageSize + page) != NRF_SUCCESS)
    return false;
  const auto* words = reinterpret_cast<const uint32_t*>(
      kBaseAddress + page * kPageSize);
  for (uint32_t index = 0; index < kPageSize / sizeof(uint32_t); ++index)
    if (words[index] != UINT32_MAX) return false;
  return true;
}
}  // namespace orun_tlp
