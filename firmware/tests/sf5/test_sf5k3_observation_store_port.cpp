#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <vector>

#include "sf5k3_observation_store_port.h"

using namespace orun_tlp;
using namespace orun_tlp::sf5k3_candidate;
namespace {
class FakeFlash final : public FlashBackend {
 public:
  explicit FakeFlash(uint16_t pages)
      : bytes(size_t(pages) * osf::kPageSize, 0xFFU), pages_(pages) {}
  bool begin() override { return true; }
  bool read(uint32_t off, void* p, size_t n) const override {
    if (!p || n == 0U || uint64_t(off) + n > bytes.size()) return false;
    if (failed_reads_) { --failed_reads_; return false; }
    memcpy(p, bytes.data() + off, n);
    return true;
  }
  FlashOpResult program(uint32_t off, const void* p, size_t n) override {
    if (!p || !n || (off & 3U) || (n & 3U) ||
        uint64_t(off) + n > bytes.size()) return FlashOpResult::kFailed;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i)
      if (bytes[off + i] != 0xFFU) return FlashOpResult::kFailed;
    for (size_t i = 0; i < n; ++i) bytes[off + i] &= b[i];
    ++writes;
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= pages_) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * osf::kPageSize, 0xFF,
           osf::kPageSize);
    return FlashOpResult::kDone;
  }
  void failOneRead() const { failed_reads_ = 1U; }
  std::vector<uint8_t> bytes;
  uint32_t writes = 0U;
 private:
  uint16_t pages_;
  mutable unsigned failed_reads_ = 0U;
};

class FixedIncarnation final : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& v) override {
    v = 0x123456789ABCDEF0ULL;
    return true;
  }
};

void settle(ObservationStore& s) {
  for (unsigned i = 0U; i < 1000U && s.busy(); ++i) s.poll();
  assert(!s.busy());
}
void prep(ObservationStore& s) {
  assert(s.requestMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(s);
  bool done = false;
  assert(s.takeMaintenanceResult(done) && done);
  assert(s.requestControlMaintenance() ==
         ObservationStore::MaintenanceResult::kStarted);
  settle(s);
  assert(s.takeControlMaintenanceResult(done) && done);
}
struct Fixture {
  FakeFlash flash{6U};
  FixedIncarnation incarnation;
  ObservationStore store{flash, 6U, &incarnation};
  osf::RecordIdentity ids[4]{};
  osc::ExactObject expected[4]{};
  Fixture() {
    assert(store.begin(0x12345678ULL));
    prep(store);
    for (size_t i = 0U; i < 4U; ++i) {
      uint8_t p[osf::kPeriodicPayloadSizeV1]{};
      for (size_t k = 0U; k < sizeof(p); ++k)
        p[k] = static_cast<uint8_t>(i * 7U + k);
      assert(store.requestAppend(osf::RecordKind::kPeriodic, 1U,
                 p, sizeof(p)) == ObservationStore::AppendResult::kStarted);
      settle(store);
      bool ok = false;
      ObservationStore::Handle h{};
      assert(store.takeAppendResult(ok, h) && ok);
      ids[i] = h.identity;
      expected[i].identity = h.identity;
      expected[i].record_kind = osf::RecordKind::kPeriodic;
      expected[i].object_size = 100U;
      for (size_t b = 0U; b < 100U; ++b)
        expected[i].object[b] = static_cast<uint8_t>(b + i * 19U);
      assert(store.requestPutExactObject(expected[i]) ==
             ObservationStore::ControlWriteResult::kStarted);
      settle(store);
      assert(store.takeControlWriteResult(ok) && ok);
    }
  }
};

void drain(ReleaseProgress& p, ObservationStore& s) {
  for (unsigned n = 0U; n < 300U; ++n) {
    const Status result = p.tick();
    if (result == Status::kFinished) return;
    assert(result != Status::kFault);
    s.poll();
  }
  assert(false && "release progress timed out");
}
void assertRelease(ObservationStore& s, const osf::RecordIdentity& id,
                   bool released) {
  ObservationStore::Record record{};
  assert(s.lookup(id, record) == ObservationStore::LookupResult::kFound);
  assert(record.released == released);
  osc::ExactObject cached{};
  const auto look = s.findExactObject(id, cached);
  assert(look == (released ? ObservationStore::ControlLookupResult::kNone
                           : ObservationStore::ControlLookupResult::kFound));
}

void successAndSelective() {
  Fixture f;
  ObservationStorePort port(f.store, f.expected, 4U);
  assert(port.ready());
  ReleaseProgress p(port);
  assert(p.beginHostPreflighted(f.ids, 4U));
  drain(p, f.store);
  for (const auto& id : f.ids) assertRelease(f.store,id,true);

  Fixture selective;
  ObservationStorePort ps(selective.store,selective.expected,4U);
  osf::RecordIdentity chosen[2]={selective.ids[0],selective.ids[2]};
  ReleaseProgress s(ps);
  assert(s.beginHostPreflighted(chosen,2U));
  drain(s,selective.store);
  for (size_t i=0U;i<4U;++i)
    assertRelease(selective.store,selective.ids[i],i==0U||i==2U);
}

void powerCutAndFreshSubsetACK() {
  Fixture f;
  ObservationStorePort first(f.store,f.expected,4U);
  ReleaseProgress p(first);
  assert(p.beginHostPreflighted(f.ids,4U));
  // Commit exactly the first release then reboot before the remaining 3.
  for (unsigned n=0U;n<100U;++n) {
    if (p.completed() >= 1U) break;
    assert(p.tick()!=Status::kFault);
    f.store.poll();
  }
  assert(p.completed() == 1U);
  assertRelease(f.store,f.ids[0],true);
  for (size_t i=1U;i<4U;++i) assertRelease(f.store,f.ids[i],false);
  ObservationStore recovered(f.flash,6U,&f.incarnation);
  assert(recovered.begin(0x12345678ULL));
  ObservationStorePort old(recovered,f.expected,4U);
  ReleaseProgress rejected(old);
  assert(!rejected.beginHostPreflighted(f.ids,4U));
  osf::RecordIdentity rest[3]={f.ids[1],f.ids[2],f.ids[3]};
  ObservationStorePort fresh(recovered,f.expected+1,3U);
  ReleaseProgress after(fresh);
  assert(after.beginHostPreflighted(rest,3U));
  drain(after,recovered);
  for (const auto& id : f.ids) assertRelease(recovered,id,true);
}

void mismatchedAndUnreadableFailClosed() {
  Fixture f;
  osc::ExactObject bad[4];
  for(size_t i=0U;i<4U;++i) bad[i]=f.expected[i];
  bad[2].object[10] ^= 1U;
  ObservationStorePort mismatch(f.store,bad,4U);
  ReleaseProgress reject(mismatch);
  assert(!reject.beginHostPreflighted(f.ids,4U));
  for (const auto& id : f.ids) assertRelease(f.store,id,false);
  ObservationStorePort good(f.store,f.expected,4U);
  f.flash.failOneRead();
  ReleaseProgress broken(good);
  assert(!broken.beginHostPreflighted(f.ids,4U));
  for (const auto& id : f.ids) assertRelease(f.store,id,false);
}
}
int main() {
  successAndSelective();
  powerCutAndFreshSubsetACK();
  mismatchedAndUnreadableFailClosed();
  puts("SF5K3A REAL ObservationStore host adapter: PASS");
}
