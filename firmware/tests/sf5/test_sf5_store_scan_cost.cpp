// Store scan cost on the custody paths (real ProductRecordStore/ObservationStore
// on a fake NOR flash).
//
// Owner bench logs (SF5L4): the collar's custody-ACK verification grew by
// ~260 ms per send because snapshotRetainedExact()/exactObjectCount() ran one
// FULL data-page scan per exact-object control. They now resolve every backing
// record in one pass. This test pins:
//   1. results: the snapshot is exactly the cached-and-unreleased set, sorted,
//      and the count matches, through many cache/release cycles including
//      control-page rotation;
//   2. cost: flash reads per call stay bounded instead of growing per cycle;
//   3. CRC-32: the table implementation equals the bitwise one.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <utility>

#include "crc32_ieee.h"

#define main sf5f_original_host_main
#include "test_sf5f_product_record_store.cpp"
#undef main

namespace {

uint32_t bitwiseCrc(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (unsigned b = 0; b < 8U; ++b)
      c = (c >> 1U) ^ (0xEDB88320U & (0U - (c & 1U)));
  }
  return c ^ 0xFFFFFFFFU;
}

void crcEquivalence() {
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert(orun_tlp::crc32Ieee(check, sizeof(check)) == 0xCBF43926U);
  assert(orun_tlp::crc32Ieee(check, 0) == 0U);
  uint8_t buf[300];
  uint32_t x = 0x12345678U;
  for (unsigned round = 0; round < 200U; ++round) {
    for (auto& b : buf) {
      x = x * 1664525U + 1013904223U;
      b = static_cast<uint8_t>(x >> 24);
    }
    const size_t n = round % sizeof(buf);
    assert(orun_tlp::crc32Ieee(buf, n) == bitwiseCrc(buf, n));
  }
}

void cacheExact(ProductRecordStore& records, uint32_t sequence) {
  const auto exact = protectedFixture(records, sequence);
  assert(records.submitExactCache(exact) ==
         ProductRecordStore::CacheSubmit::kStarted);
  settleExactCache(records, exact, true);
}

void releaseOne(FakeFlash& flash, FixedIncarnation& source,
                const osf::RecordIdentity& id) {
  ObservationStore owner(flash, kPages, &source);
  assert(owner.begin(kDevice));
  assert(owner.requestRelease(id));
  for (unsigned n = 0U; n < 2000U && owner.busy(); ++n) owner.poll();
  bool committed = false;
  assert(owner.takeReleaseResult(committed) && committed);
}

// The truth: cached identities whose record is still retained and unreleased.
using Key = std::pair<uint64_t, uint32_t>;

void checkSnapshot(const ProductRecordStore& records,
                   const std::set<Key>& pending, unsigned& reads_snapshot,
                   unsigned& reads_count, const FakeFlash& flash) {
  osc::ExactObject objects[osf::kMaxExactCustodyObjects]{};
  uint16_t count = 0;
  unsigned before = flash.read_calls;
  assert(records.snapshotCustodyCandidates(objects, count));
  reads_snapshot = flash.read_calls - before;
  assert(count == pending.size());
  auto it = pending.begin();
  for (uint16_t i = 0; i < count; ++i, ++it) {  // ascending, exact set
    assert(objects[i].identity.incarnation == it->first);
    assert(objects[i].identity.sequence == it->second);
    assert(objects[i].object_size == tlp::kPeriodicFrameSize);
  }
  uint16_t occupied = 0;
  before = flash.read_calls;
  assert(records.store().exactObjectCount(occupied));
  reads_count = flash.read_calls - before;
  assert(occupied == pending.size());
}

void cyclesStayCorrectAndBounded() {
  FakeFlash flash;
  FixedIncarnation source;
  std::set<Key> pending;
  uint64_t incarnation = 0;
  uint32_t next_seq = 1;
  {
    ProductRecordStore records(flash, kPages, &source);
    records.begin(kDevice, kTracker);
    // A realistically full-ish store: several data pages to scan.
    for (; next_seq <= 200U; ++next_seq) assert(store(records, next_seq).stored);
    incarnation = records.store().incarnation();
  }
  uint32_t oldest_unsent = 1;
  unsigned first_snapshot = 0, first_count = 0;
  unsigned max_snapshot = 0, max_count = 0;
  for (unsigned cycle = 0; cycle < 120U; ++cycle) {
    {
      ProductRecordStore records(flash, kPages, &source);
      records.begin(kDevice, kTracker);
      assert(records.mounted());
      // Keep up to three objects outstanding (unanswered), like a collar
      // that lost some ACKs; cache the next oldest record.
      if (pending.size() < 3U) {
        cacheExact(records, oldest_unsent);
        pending.insert(Key(incarnation, oldest_unsent));
        ++oldest_unsent;
      }
      if (cycle % 3U == 0U) {  // production continues
        assert(store(records, next_seq++).stored);
      }
      unsigned rs = 0, rc = 0;
      checkSnapshot(records, pending, rs, rc, flash);
      if (cycle == 3U) { first_snapshot = rs; first_count = rc; }
      if (cycle >= 3U) {
        if (rs > max_snapshot) max_snapshot = rs;
        if (rc > max_count) max_count = rc;
      }
    }
    // ACK for the oldest outstanding object (every cycle but every 4th).
    if (cycle % 4U != 3U && !pending.empty()) {
      osf::RecordIdentity id{};
      id.incarnation = pending.begin()->first;
      id.sequence = pending.begin()->second;
      releaseOne(flash, source, id);
      pending.erase(pending.begin());
    }
  }
  printf("store scan cost: snapshot reads first=%u max=%u, count reads "
         "first=%u max=%u over 120 cycles\n",
         first_snapshot, max_snapshot, first_count, max_count);
  // Before the fix each call did one full data scan per exact control plus
  // one control-page re-read per control (here: 296 -> 1069 reads, growing
  // until the control page rotated). Now it is ONE control pass and ONE data
  // pass; only the data pages written meanwhile may add reads.
  assert(max_snapshot <= first_snapshot + first_snapshot / 2U);
  assert(max_count <= first_count + first_count / 2U);
}

}  // namespace

int main() {
  crcEquivalence();
  cyclesStayCorrectAndBounded();
  puts("SF5 store scan cost: one-pass custody snapshot/count, CRC-32 table: "
       "PASS");
}
