// KQ05 PC-ONLY: the ACTUAL ObservationStore over 32+7 fake NOR pages.
// The host segment joiner is not permitted in production firmware.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "kq05_segmented_flash_host.h"
#include "observation_store.h"

namespace k = orun_tlp::kq05_segmented_host;
namespace osf = orun_tlp::observation_store_format;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;
using orun_tlp::ObservationStore;
using orun_tlp::ObservationIncarnationSource;

namespace {

constexpr uint64_t kDeviceId = UINT64_C(0x1020304050607080);
constexpr uint64_t kIncarnation = UINT64_C(0xBA09120000000001);

struct Nor final : FlashBackend {
  explicit Nor(uint32_t pages)
      : bytes(static_cast<size_t>(pages) * k::kPageSize, 0xFFU),
        page_count(pages) {}

  std::vector<uint8_t> bytes;
  const uint32_t page_count;
  uint32_t programs = 0U;
  uint32_t erases = 0U;
  bool corrupt = false;

  bool begin() override { return !corrupt; }
  bool read(uint32_t offset, void* output, size_t size) const override {
    if (output == nullptr || size == 0U ||
        offset >= bytes.size() || size > bytes.size() - offset)
      return false;
    memcpy(output, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* input,
                        size_t size) override {
    if (input == nullptr || size == 0U || (offset & 3U) != 0U ||
        (size & 3U) != 0U || offset >= bytes.size() ||
        size > bytes.size() - offset ||
        size > k::kPageSize - (offset % k::kPageSize))
      return FlashOpResult::kFailed;
    const uint8_t* source = static_cast<const uint8_t*>(input);
    for (size_t i = 0U; i < size; ++i)
      if ((bytes[offset + i] & source[i]) != source[i])
        return FlashOpResult::kFailed;
    for (size_t i = 0U; i < size; ++i)
      bytes[offset + i] &= source[i];
    ++programs;
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= page_count) return FlashOpResult::kFailed;
    memset(bytes.data() + page * k::kPageSize, 0xFFU, k::kPageSize);
    ++erases;
    return FlashOpResult::kDone;
  }
};

struct FixedIncarnation final : ObservationIncarnationSource {
  uint32_t requests = 0U;
  bool generate(uint64_t& output) override {
    ++requests;
    output = kIncarnation;
    return true;
  }
};

void settle(ObservationStore& store) {
  for (unsigned i = 0U; i < 100U && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

void prepareData(ObservationStore& store) {
  assert(store.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool completed = false;
  assert(store.takeMaintenanceResult(completed) && completed);
  assert(store.hasPreparedDataPage());
}

void prepareControl(ObservationStore& store) {
  assert(store.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(store);
  bool completed = false;
  assert(store.takeControlMaintenanceResult(completed) && completed);
}

ObservationStore::Handle append(ObservationStore& store, uint32_t seq) {
  uint8_t payload[osf::kPeriodicPayloadSizeV1]{};
  for (size_t i = 0U; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>((seq + i * 19U) & 0xFFU);
  assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                             payload, sizeof(payload)) ==
         ObservationStore::AppendResult::kStarted);
  settle(store);
  bool committed = false;
  ObservationStore::Handle handle{};
  assert(store.takeAppendResult(committed, handle) && committed);
  assert(handle.identity.incarnation == kIncarnation);
  assert(handle.identity.sequence == seq);
  return handle;
}

void testObservationAcrossPhysicalGap() {
  Nor primary(k::kPrimaryPages);
  Nor extra(k::kSupplementalPages);
  k::SegmentedFlash logical(primary, extra, true, true);
  FixedIncarnation identity;
  ObservationStore store(logical, k::kTotalPages, &identity);
  assert(store.begin(kDeviceId) && store.ready() && !store.faulted());
  assert(identity.requests == 1U);
  prepareControl(store);

  // First two pages are control journals; 30 data pages fit in 128 KiB.
  const uint32_t primary_data_pages =
      k::kPrimaryPages - osf::kControlPageCount;
  const uint32_t primary_records =
      primary_data_pages * osf::kDataRecordsPerPage;
  static_assert(k::kPrimaryPages == 32U &&
                osf::kControlPageCount == 2U, "layout changed");
  ObservationStore::Handle first{}, last_primary{};
  uint32_t sequence = 0U;
  for (uint32_t page = 0U; page < primary_data_pages; ++page) {
    prepareData(store);
    for (uint32_t slot = 0U; slot < osf::kDataRecordsPerPage; ++slot) {
      ++sequence;
      const ObservationStore::Handle handle = append(store, sequence);
      if (sequence == 1U) first = handle;
      last_primary = handle;
    }
  }
  assert(sequence == primary_records && primary_records == 1260U);
  assert(first.page == 2U && first.identity.sequence == 1U);
  assert(last_primary.page == 31U &&
         last_primary.identity.sequence == primary_records);
  assert(extra.programs == 0U && extra.erases == 0U);

  // The *same* ObservationStore allocates the NEXT logical ID and writes
  // the next page through the seam. No second sequence allocator exists.
  prepareData(store);
  const ObservationStore::Handle overflow = append(
      store, primary_records + 1U);
  assert(overflow.page == 32U && overflow.slot == 0U);
  assert(overflow.identity.sequence == 1261U);
  assert(extra.programs > 0U && extra.erases == 0U);
  const uint32_t primary_writes_before_release = primary.programs;

  ObservationStore::Record read{};
  assert(store.lookup(overflow.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(!read.released && read.handle.page == 32U);

  // Non-prefix selective release of the overflow observation must touch
  // ONLY the supplemental NOR and leave the oldest primary record live.
  assert(store.requestRelease(overflow.identity));
  settle(store);
  bool released = false;
  assert(store.takeReleaseResult(released) && released);
  assert(primary.programs == primary_writes_before_release);
  assert(store.lookup(overflow.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(read.released);
  assert(store.lookup(first.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(!read.released);

  ObservationStore reboot(logical, k::kTotalPages, &identity);
  assert(reboot.begin(kDeviceId) && reboot.ready() && !reboot.faulted());
  assert(identity.requests == 1U);  // no new incarnation after reboot
  assert(reboot.incarnation() == kIncarnation);
  osf::RecordIdentity next{};
  assert(reboot.peekNextIdentity(next));
  assert(next.incarnation == kIncarnation && next.sequence == 1262U);
  assert(reboot.lookup(overflow.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(read.released && read.handle.page == 32U);
  assert(reboot.lookup(first.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(!read.released);
  assert(reboot.oldestRetained(read) ==
         ObservationStore::LookupResult::kFound);
  assert(read.handle.identity.sequence == 1U);

  const ObservationStore::Handle continued = append(reboot, 1262U);
  assert(continued.page == 32U && continued.slot == 1U);
  assert(primary.erases == 0U && extra.erases == 0U);

  ObservationStore second_reboot(logical, k::kTotalPages, &identity);
  assert(second_reboot.begin(kDeviceId) &&
         second_reboot.ready() && !second_reboot.faulted());
  assert(second_reboot.peekNextIdentity(next));
  assert(next.sequence == 1263U);
  assert(second_reboot.lookup(continued.identity, read) ==
         ObservationStore::LookupResult::kFound);
  assert(!read.released && read.handle.page == 32U);
}

void testWrongOwnerAndLegacyRefuseWithoutWriting() {
  Nor primary(k::kPrimaryPages);
  Nor extra(k::kSupplementalPages);
  extra.bytes[0] = 'H'; extra.bytes[1] = 'I';
  extra.bytes[2] = 'S'; extra.bytes[3] = 'T';
  k::SegmentedFlash logical(primary, extra, true, true);
  FixedIncarnation id;
  ObservationStore foreign(logical, k::kTotalPages, &id);
  assert(!foreign.begin(kDeviceId) || foreign.faulted());
  assert(extra.programs == 0U && primary.programs == 0U);
  assert(primary.erases == 0U && extra.erases == 0U);
  Nor clean_primary(k::kPrimaryPages);
  Nor clean_extra(k::kSupplementalPages);
  k::SegmentedFlash no_legacy_exclusion(
      clean_primary, clean_extra, false, true);
  ObservationStore blocked(no_legacy_exclusion, k::kTotalPages, &id);
  assert(!blocked.begin(kDeviceId));
  assert(clean_primary.programs == 0U && clean_extra.programs == 0U);
}

}  // namespace

int main() {
  testObservationAcrossPhysicalGap();
  testWrongOwnerAndLegacyRefuseWithoutWriting();
  puts("KQ05 tracker real OBS1 39-page overflow / one sequence / selective release / reboot: PASS (HOST ONLY)");
  puts("NO REAL FLASH, NO LEGACY HISTORY ERASE, NO AUTHENTICATED BATCH ACK, NO DEVICE UPLOAD");
  return 0;
}
