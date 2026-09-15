// Compile the ACTUAL production backend. Only Nordic API/registers are mocked.
// Linux fixed-address mapping represents its memory-mapped flash partition.
// This verifies the disabled synchronous contract, not physical endurance.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <Arduino.h>
#include <nrf_sdm.h>
#include "storage_config.h"
#include "position_flow.h"
#include "gnss_manager.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
TestFicr ficr{kPageSize, 256};
TestUicr uicr{{0xF4000}};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;
bool enabled = false, query_error = false, mismatch = false;
uint32_t api_result = NRF_SUCCESS;
unsigned programs = 0, erases = 0, sends = 0;
uint32_t sd_softdevice_is_enabled(uint8_t* out) {
  *out = enabled;
  return query_error ? NRF_ERROR_BUSY : NRF_SUCCESS;
}
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t count) {
  ++programs;
  assert(!enabled);
  const uintptr_t address = reinterpret_cast<uintptr_t>(dst);
  assert(address % 4 == 0 && reinterpret_cast<uintptr_t>(src) % 4 == 0);
  assert(count && address >= kBaseAddress && address + count * 4 <= kBaseAddress + kRegionSize);
  assert(address / kPageSize == (address + count * 4 - 1) / kPageSize);
  if (api_result != NRF_SUCCESS || mismatch) return api_result;
  for (uint32_t i = 0; i < count; ++i) {
    assert((dst[i] & src[i]) == src[i]);
    dst[i] &= src[i];
  }
  return NRF_SUCCESS;
}
uint32_t sd_flash_page_erase(uint32_t page) {
  ++erases;
  assert(!enabled);
  assert(page >= kBaseAddress / kPageSize && page < (kBaseAddress + kRegionSize) / kPageSize);
  if (api_result != NRF_SUCCESS || mismatch) return api_result;
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF, kPageSize);
  return NRF_SUCCESS;
}

bool RadioManager::begin(SequenceSource& source) {
  sequences_ = &source;
  device_id_ = 1;
  return true;
}
bool RadioManager::canSend() const { return true; }
bool RadioManager::encodePosition(const GnssFix& fix, uint8_t* out, uint64_t& id) {
  uint32_t seq;
  if (!sequences_->nextSequence(seq, id)) return false;
  const tlp::PositionPacket p{1, seq, 0, fix.latitude_e7, fix.longitude_e7,
                            fix.altitude_mm, fix.hdop_x100, fix.satellites, fix.flags};
  return tlp::serializePositionPacket(p, out, tlp::kPositionPacketSize);
}
bool RadioManager::sendPositionPacket(const uint8_t*, const uint32_t*) { ++sends; return true; }

int main() {
  void* region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(region == reinterpret_cast<void*>(kBaseAddress));
  memset(region, 0xFF, kRegionSize);
  NrfHistoryFlash flash;
  assert(flash.begin());
  alignas(4) uint8_t source[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  assert(flash.program(0, source + 1, 8)); // Unaligned caller -> aligned Nordic source.
  assert(programs == 1 && erases == 0);
  assert(memcmp(region, source + 1, 8) == 0);
  assert(!flash.program(0, source, 4)); // Already programmed destination.
  assert(!flash.program(1, source, 4));
  assert(!flash.program(8, source, 3));
  assert(!flash.program(kPageSize - 4, source, 8));
  assert(!flash.program(kRegionSize - 4, source, 8));
  assert(!flash.erasePage(kPageCount));
  assert(programs == 1 && erases == 0);

  enabled = true;
  assert(!flash.program(8, source, 4));
  assert(!flash.erasePage(0));
  NrfHistoryFlash unsupported;
  assert(!unsupported.begin());
  assert(programs == 1 && erases == 0);
  enabled = false;
  query_error = true;
  assert(!flash.program(8, source, 4) && !flash.erasePage(0));
  assert(programs == 1 && erases == 0);
  query_error = false;
  api_result = NRF_ERROR_BUSY;
  assert(!flash.program(8, source, 4) && !flash.erasePage(0));
  api_result = NRF_SUCCESS;
  mismatch = true;
  assert(!flash.program(8, source, 4) && !flash.erasePage(0));
  mismatch = false;
  assert(flash.erasePage(0));

  HistoryStore store(flash);
  assert(store.begin(1));
  while (store.busy()) store.poll();
  RadioManager radio;
  assert(radio.begin(store));
  PositionFlow flow(store, radio);
  const GnssFix fix{0, 410000000, 290000000, 10, 100, 8, 5};
  assert(flow.acceptFix(fix, 0));
  const auto append_programs = programs, append_erases = erases;
  store.poll();
  assert(flow.update(1, false) == PositionFlow::Event::kStored);
  assert(programs == append_programs + 2 && erases == append_erases);
  HistoryStore::Record committed;
  assert(store.lookup(1, committed));
  assert(flow.acceptFix(fix, 2));
  const auto before_programs = programs, before_erases = erases;
  enabled = true; // Guard must also work after successful initialization.
  store.poll();
  assert(flow.update(3) == PositionFlow::Event::kStorageFailure);
  assert(!flow.pending() && !store.ready() && sends == 0);
  assert(!flow.acceptFix(fix, 4));
  assert(programs == before_programs && erases == before_erases);
  enabled = false;
  HistoryStore recovered(flash);
  assert(recovered.begin(1));
  while (recovered.busy()) recovered.poll();
  assert(recovered.lookup(1, committed) && recovered.count() == 1);
  assert(munmap(region, kRegionSize) == 0);
  puts("M4 production backend synchronous/SoftDevice guard checks: PASS");
}
