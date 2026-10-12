// SF5F step 1: ProductRecordStore, the tracker's durable PERIODIC owner on
// the product region (product_record_store.h). Portable: a fake 32-page
// flash, synchronous or asynchronous like the gate.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "product_record_store.h"
#include "storage_config.h"
#include "tlp_v2_product_secure.h"

using namespace orun_tlp;
namespace pr = orun_tlp::product_region;

namespace {

constexpr uint16_t kPages = storage_config::kProductRegionPages;
constexpr uint32_t kPage = storage_config::kPageSize;

struct Op {
  bool erase;
  uint32_t offset;
  std::vector<uint8_t> data;
};

class FakeFlash : public FlashBackend {
 public:
  FakeFlash() : bytes(size_t(kPages) * kPage, 0xFF) {}

  bool begin() override { return true; }
  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || uint64_t(offset) + size > bytes.size()) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    if (pending_ || data == nullptr || size == 0 || (offset & 3U) ||
        (size & 3U) || uint64_t(offset) + size > bytes.size())
      return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFF) return FlashOpResult::kFailed;
    const auto* src = static_cast<const uint8_t*>(data);
    if (record) ops.push_back(Op{false, offset, {src, src + size}});
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    return finish();
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (pending_ || page >= kPages) return FlashOpResult::kFailed;
    if (record) ops.push_back(Op{true, page * kPage, {}});
    memset(bytes.data() + size_t(page) * kPage, 0xFF, kPage);
    return finish();
  }
  FlashOpResult pollPending() override {
    if (!pending_) return FlashOpResult::kFailed;
    if (polls_left_ > 0) {
      --polls_left_;
      return FlashOpResult::kPending;
    }
    pending_ = false;
    return FlashOpResult::kDone;
  }

  std::vector<uint8_t> bytes;
  std::vector<Op> ops;
  bool record = false;
  bool async = false;

 private:
  FlashOpResult finish() {
    if (!async) return FlashOpResult::kDone;
    pending_ = true;
    polls_left_ = 2;
    return FlashOpResult::kPending;
  }
  bool pending_ = false;
  unsigned polls_left_ = 0;
};

class FixedIncarnation : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    value = 0x0102030405060708ULL;
    return true;
  }
};

constexpr uint64_t kDevice = 0x0E8ADE7E71531AA3ULL;
const pr::Jobs kTracker{false, true};

PeriodicRecordInputs period(uint32_t n) {
  PeriodicRecordInputs in;
  in.period_duration_seconds = 3600;
  in.location.valid = true;
  in.location.latitude_e7 = 376102718 + int32_t(n);
  in.location.longitude_e7 = 280545028;
  in.location.hdop_x100 = 120;
  in.location.satellites = 7;
  in.location.age_valid = true;
  in.location.age_seconds = 3;
  in.battery_valid = true;
  in.battery_mv = 4100;
  in.battery_state = 1;
  in.health.valid = true;
  return in;
}

// Submits one period and drives the owner until its outcome arrives.
ProductRecordStore::Outcome store(ProductRecordStore& records, uint32_t n) {
  assert(records.submitPeriodic(period(n)) ==
         ProductRecordStore::Submit::kAccepted);
  ProductRecordStore::Outcome outcome;
  for (unsigned i = 0; i < 20000U; ++i) {
    records.poll();
    if (records.takeOutcome(outcome)) return outcome;
  }
  assert(false && "no outcome");
  return outcome;
}

// The stored plaintext carries exactly the store's identity.
void checkStored(const ProductRecordStore& records, uint32_t sequence,
                 uint32_t n) {
  ObservationStore::Record record;
  osf::RecordIdentity identity;
  identity.incarnation = records.store().incarnation();
  identity.sequence = sequence;
  assert(records.store().lookup(identity, record) ==
         ObservationStore::LookupResult::kFound);
  tlp::PeriodicObservationPlaintext decoded{};
  assert(tlp::decodePeriodicPlaintext(record.payload, record.payload_size,
                                      &decoded) ==
         tlp::ProductSecureCodecResult::kOk);
  assert(decoded.record_incarnation == identity.incarnation);
  assert(decoded.record_sequence == sequence);
  assert(decoded.latitude_e7 == 376102718 + int32_t(n));
}

void testStoreAndRemount(bool async) {
  FakeFlash flash;
  flash.async = async;
  FixedIncarnation source;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(records.regionReadable() && records.mounted());
    assert(records.decision() == pr::MountDecision::kMount);
    assert(records.census().erased == kPages);
    for (uint32_t n = 1; n <= 3; ++n) {
      const auto outcome = store(records, n);
      assert(outcome.stored && outcome.sequence == n);
      assert(outcome.incarnation == 0x0102030405060708ULL);
      checkStored(records, n, n);
    }
    assert(records.diagnostics().stored == 3 &&
           records.diagnostics().failed == 0);
    // A second period while the first is still pending is refused.
    assert(records.submitPeriodic(period(9)) ==
           ProductRecordStore::Submit::kAccepted);
    assert(records.submitPeriodic(period(10)) ==
           ProductRecordStore::Submit::kBusy);
    assert(records.diagnostics().dropped_busy == 1);
    ProductRecordStore::Outcome outcome;
    for (unsigned i = 0; i < 1000U && !records.takeOutcome(outcome); ++i)
      records.poll();
    assert(outcome.stored && outcome.sequence == 4);
  }
  flash.async = false;  // reboot: mount runs before SoftDevice
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted() && records.census().observation == 1);
  flash.async = async;
  const auto outcome = store(records, 5);
  assert(outcome.stored && outcome.sequence == 5);
  checkStored(records, 2, 2);
}

void testOwnersAndPrepare() {
  FakeFlash flash;
  FixedIncarnation source;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, pr::Jobs{false, false});
    assert(!records.mounted() &&
           records.decision() == pr::MountDecision::kNoOwner);
    assert(records.submitPeriodic(period(1)) ==
           ProductRecordStore::Submit::kNotMounted);
  }
  {
    // A Gateway owns the region for custody; this owner does not mount it.
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, pr::Jobs{true, true});
    assert(records.owner() == pr::Owner::kCustody && !records.mounted());
  }

  // Leftovers (old images, test markers) are refused until prepared.
  flash.bytes[7U * kPage + 100U] = 0x00;
  flash.bytes[20U * kPage] = 0x53;
  flash.async = true;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    assert(!records.mounted());
    assert(records.decision() == pr::MountDecision::kNeedsPreparation);
    assert(records.census().unknown == 2);
    assert(records.requestPrepare());
    assert(!records.requestPrepare());  // already running
    bool success = false;
    unsigned guard = 0;
    while (!records.takePrepareResult(success) && guard++ < 10000U)
      records.poll();
    assert(success);
    for (uint8_t b : flash.bytes) assert(b == 0xFF);
  }
  flash.async = false;
  ProductRecordStore records(flash, kPages, &source);
  records.begin(kDevice, kTracker);
  assert(records.mounted());
  const auto outcome = store(records, 1);
  assert(outcome.stored && outcome.sequence == 1);
  // A healthy mounted store is never erased by PREPARE.
  assert(!records.requestPrepare());

  // Pages of the other store: refused, and PREPARE refuses too.
  FakeFlash foreign;
  const uint8_t custody_magic[4] = {'O', 'C', 'Q', '1'};
  memcpy(foreign.bytes.data() + 3U * kPage, custody_magic, 4);
  ProductRecordStore other(foreign, kPages, &source);
  other.begin(kDevice, kTracker);
  assert(other.decision() == pr::MountDecision::kForeignStore);
  assert(!other.mounted() && !other.requestPrepare());
}

// Long run through page rotation and control compaction; then a reset at
// every single write in the second half (the write left half done), each
// followed by a remount that must store the next period.
void testFillAndResetEverywhere() {
  FakeFlash flash;
  FixedIncarnation source;
  constexpr uint32_t kRecords = 2000;
  std::vector<uint8_t> start = flash.bytes;
  flash.record = true;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    for (uint32_t n = 1; n <= kRecords; ++n) {
      const auto outcome = store(records, n);
      assert(outcome.stored && outcome.sequence == n);
    }
    assert(records.store().diagnostics().pages_reclaimed > 10U);
    assert(records.store().diagnostics().control_pages_compacted > 0U);
    assert(records.diagnostics().failed == 0U);
  }
  flash.record = false;
  const std::vector<Op> ops = flash.ops;

  FakeFlash replay;
  replay.bytes = start;
  const size_t first_cut = ops.size() / 2;
  unsigned cuts = 0;
  for (size_t k = 0; k < ops.size(); ++k) {
    const Op& op = ops[k];
    if (k >= first_cut) {
      FakeFlash cut;
      cut.bytes = replay.bytes;
      if (!op.erase) {
        const size_t half = (op.data.size() / 2U) & ~size_t(3U);
        for (size_t i = 0; i < half; ++i) cut.bytes[op.offset + i] &= op.data[i];
      }
      ProductRecordStore records(cut, kPages, &source);
      records.begin(kDevice, kTracker);
      assert(records.mounted() && !records.store().faulted());
      const auto outcome = store(records, 1);
      assert(outcome.stored);
      ++cuts;
    }
    if (op.erase)
      memset(replay.bytes.data() + op.offset, 0xFF, kPage);
    else
      for (size_t i = 0; i < op.data.size(); ++i)
        replay.bytes[op.offset + i] &= op.data[i];
  }
  assert(replay.bytes == flash.bytes);
  printf("  reset/torn write at %u write points recovered\n", cuts);
}

}  // namespace

int main() {
  testStoreAndRemount(false);
  testStoreAndRemount(true);
  testOwnersAndPrepare();
  testFillAndResetEverywhere();
  puts("SF5F ProductRecordStore checks: PASS");
}
