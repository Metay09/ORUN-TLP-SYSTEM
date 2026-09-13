#include "flash_backend.h"
#include "storage_config.h"
#include <Arduino.h>
#include <flash/flash_nrf5x.h>
#include <string.h>

extern uint32_t __flash_arduino_end[];
namespace orun_tlp {
using namespace storage_config;
static bool inBounds(uint32_t offset, size_t size) { return offset <= kRegionSize && size <= kRegionSize-offset; }
bool NrfHistoryFlash::begin() {
  const uint32_t boot = NRF_UICR->NRFFW[0];
  ready_ = reinterpret_cast<uintptr_t>(__flash_arduino_end) == kBaseAddress &&
      NRF_FICR->CODEPAGESIZE == kPageSize && NRF_FICR->CODESIZE == 256 &&
      (boot == UINT32_MAX || boot >= kBaseAddress + kRegionSize);
  return ready_;
}
bool NrfHistoryFlash::read(uint32_t offset, void* data, size_t size) const {
  return ready_ && data && inBounds(offset,size) &&
      flash_nrf5x_read(data,kBaseAddress+offset,size) == int(size);
}
bool NrfHistoryFlash::program(uint32_t offset, const void* data, size_t size) {
  if(!ready_ || !data || !size || !inBounds(offset,size)) return false;
  uint8_t previous[storage_config::kPageHeaderSize];
  if(size > sizeof(previous) || !read(offset,previous,size)) return false;
  for(size_t i=0;i<size;++i) if(previous[i] != 0xFF) return false;
  // Core flash_nrf5x_write owns SoftDevice-on asynchronous completion; flush
  // makes its page cache durable before this method reports success.
  if(flash_nrf5x_write(kBaseAddress+offset,data,size) != int(size)) return false;
  flash_nrf5x_flush();
  uint8_t verify[storage_config::kPageHeaderSize];
  return read(offset,verify,size) && !memcmp(verify,data,size);
}
bool NrfHistoryFlash::erasePage(uint32_t page) {
  return ready_ && page < kPageCount && flash_nrf5x_erase(kBaseAddress + page*kPageSize);
}
}  // namespace orun_tlp
