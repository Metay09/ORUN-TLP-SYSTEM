#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "product_send_owner.h"

using namespace orun_tlp;

namespace {

struct Fake : ProductSendPort {
  bool secure = true;
  bool owns = true;
  bool radio = true;
  bool accept_tx = true;
  bool record_present = true;
  bool cached_present = false;
  bool pending = false;
  bool durable_result = true;
  bool read_fails = false;
  bool cache_fails = false;
  bool seal_fails = false;
  bool seal_unavailable = false;
  uint32_t record_reads = 0;
  uint32_t seal_attempts = 0;
  bool change_cached_between_reads = false;
  uint32_t cache_reads = 0;
  uint32_t seals = 0;
  uint32_t puts = 0;
  uint32_t tx = 0;
  uint8_t last_sent[100]{};
  ProductSendExact cache{};

  bool securityReady() const override { return secure; }
  ProductSendRead oldestPeriodic(ProductSendPeriodic& out) override {
    ++record_reads;
    if (read_fails) return ProductSendRead::kUnavailable;
    if (!record_present) return ProductSendRead::kNone;
    out.identity.incarnation = 42;
    out.identity.sequence = 7;
    return ProductSendRead::kFound;
  }
  ProductSendRead cached(const ProductSendIdentity& id,
                         ProductSendExact& out) override {
    if (read_fails) return ProductSendRead::kUnavailable;
    if (!cached_present) return ProductSendRead::kNone;
    assert(sameProductSendIdentity(id, cache.identity));
    out = cache;
    if (change_cached_between_reads && cache_reads++ == 0U)
      cache.bytes[10] ^= 0x01U;
    return ProductSendRead::kFound;
  }
  ProductSendSeal sealOnce(const ProductSendPeriodic& record,
                           ProductSendExact& out) override {
    ++seal_attempts;
    if (seal_unavailable) return ProductSendSeal::kUnavailable;
    if (seal_fails) return ProductSendSeal::kFailed;
    ++seals;
    out.identity = record.identity;
    out.size = 100;
    memset(out.bytes, 0xA5, sizeof(out.bytes));
    out.bytes[27] = static_cast<uint8_t>(seals); // fake counter
    return ProductSendSeal::kOk;
  }
  ProductSendCache submitCache(const ProductSendExact& exact) override {
    ++puts;
    if (cache_fails) return ProductSendCache::kRejected;
    cache = exact;
    pending = true;
    return ProductSendCache::kStarted;
  }
  bool takeCacheResult(ProductSendIdentity& id, bool& durable) override {
    if (!pending) return false;
    pending = false;
    id = cache.identity;
    durable = durable_result;
    if (durable) cached_present = true;
    return true;
  }
  bool currentCredentialOwns(const ProductSendExact&) const override {
    return owns;
  }
  bool radioReady() const override { return radio; }
  bool transmitExact(const uint8_t* data, size_t size) override {
    assert(size == sizeof(last_sent));
    ++tx;
    memcpy(last_sent, data, size);
    return accept_tx;
  }
};

void testStoreBeforeSendAndBackoff() {
  Fake p;
  ProductSendOwner sender(p);
  sender.poll(1U); // seal + request cache; no premature TX
  assert(sender.state() == ProductSendOwner::State::kAwaitCache);
  assert(p.seals == 1 && p.puts == 1 && p.tx == 0);
  sender.poll(2U); // committed exact-byte readback then TX attempt
  assert(p.tx == 1 && sender.diagnostics().cache_committed == 1);
  assert(sender.state() == ProductSendOwner::State::kRetry);
  uint8_t original[100]{};
  memcpy(original, p.last_sent, sizeof(original));
  sender.poll(30001U);
  assert(p.tx == 1);
  sender.poll(30002U);
  assert(p.tx == 2 && p.seals == 1 && p.puts == 1);
  assert(memcmp(original, p.last_sent, sizeof(original)) == 0);
  sender.poll(90002U);
  assert(p.tx == 3 && p.seals == 1 && p.puts == 1);
}


void testBackoffReachesFifteenMinuteCap() {
  Fake p;
  ProductSendOwner sender(p);
  sender.poll(0U);     // durability still pending
  sender.poll(1U);     // first attempt; next in 30 s
  assert(p.tx == 1U);
  const uint32_t attempts[] = {
      30001U, 90001U, 210001U, 450001U, 930001U,
      1830001U, 2730001U};
  for (unsigned i = 0U; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
    sender.poll(attempts[i] - 1U);
    assert(p.tx == i + 1U);
    sender.poll(attempts[i]);
    assert(p.tx == i + 2U);
  }
  assert(p.seals == 1U && p.puts == 1U);
  assert(sender.diagnostics().tx_started == 8U);
  assert(sender.state() == ProductSendOwner::State::kRetry);
}

void testSealUnavailableThrottlesFlashScan() {
  Fake p;
  p.seal_unavailable = true;
  ProductSendOwner sender(p);
  sender.poll(0U);
  assert(p.record_reads == 1U && p.seal_attempts == 1U);
  assert(p.seals == 0U && p.puts == 0U && p.tx == 0U);
  for (unsigned now = 1U; now < 60000U; now += 17U) sender.poll(now);
  assert(p.record_reads == 1U && p.seal_attempts == 1U);
  sender.poll(60000U);
  assert(p.record_reads == 2U && p.seal_attempts == 2U);
  assert(p.seals == 0U);
  p.seal_unavailable = false;
  sender.poll(60001U);
  assert(p.record_reads == 2U && p.tx == 0U);
  sender.poll(120000U);
  assert(p.record_reads == 3U && p.seals == 1U && p.tx == 0U);
  sender.poll(120001U);
  assert(p.tx == 1U && p.puts == 1U);
}

void testCachedObjectChangesBetweenReads() {
  Fake p;
  p.cached_present = true;
  p.cache.identity.incarnation = 42U;
  p.cache.identity.sequence = 7U;
  p.cache.size = 100U;
  memset(p.cache.bytes, 0xA5, sizeof(p.cache.bytes));
  p.change_cached_between_reads = true;
  ProductSendOwner sender(p);
  sender.poll(0U);
  assert(p.cache_reads == 2U);
  assert(sender.state() == ProductSendOwner::State::kFault);
  assert(p.seals == 0U && p.puts == 0U && p.tx == 0U);
}

void testRebootUsesExactAndBurnsNoCounter() {
  Fake p;
  ProductSendOwner first(p);
  first.poll(10U);
  first.poll(11U);
  assert(p.cached_present && p.seals == 1);
  ProductSendOwner reboot(p);
  reboot.poll(100U);
  assert(p.tx == 2 && p.seals == 1 && p.puts == 1);
  assert(reboot.diagnostics().cached_recovered == 1);
}

void testSecurityAndNoCredentialLifetime() {
  Fake p;
  p.secure = false;
  ProductSendOwner unprovisioned(p);
  unprovisioned.poll(10U);
  assert(p.tx == 0 && p.seals == 0);
  p.secure = true;
  p.owns = false;
  unprovisioned.poll(11U);
  unprovisioned.poll(12U);
  assert(p.tx == 0 && unprovisioned.state() == ProductSendOwner::State::kFault);
  assert(p.seals == 1 && p.cached_present);
}

void testWriteFailuresFailClosed() {
  for (bool durable : {false, true}) {
    Fake p;
    ProductSendOwner sender(p);
    sender.poll(0U);
    p.durable_result = durable;
    if (durable) p.read_fails = true; // committed but final readback failed
    sender.poll(1U);
    assert(p.tx == 0 && p.seals == 1);
    assert(sender.state() == ProductSendOwner::State::kFault);
    p.read_fails = false;
    sender.poll(1000000U);
    assert(p.tx == 0 && p.seals == 1);
  }
  Fake p;
  p.cache_fails = true;
  ProductSendOwner sender(p);
  sender.poll(0U);
  assert(sender.state() == ProductSendOwner::State::kFault);
  assert(p.tx == 0 && p.seals == 1);
}

void testUnavailableRadioAndInvalidatedCache() {
  Fake p;
  p.radio = false;
  ProductSendOwner sender(p);
  sender.poll(0U);
  sender.poll(1U);
  assert(p.tx == 0 && p.seals == 1);
  p.radio = true;
  p.accept_tx = false;
  sender.poll(2U);
  assert(p.tx == 1 && sender.diagnostics().tx_refused == 1);
  p.accept_tx = true;
  sender.poll(30002U);
  assert(p.tx == 2 && p.seals == 1);
  p.cache.bytes[13] ^= 0x01;
  sender.poll(90002U);
  assert(sender.state() == ProductSendOwner::State::kFault);
  assert(p.tx == 2);
}

void testNoRecordAndReleasedBacking() {
  Fake p;
  p.record_present = false;
  ProductSendOwner sender(p);
  sender.poll(0U);
  assert(p.seals == 0 && p.tx == 0 && sender.state() == ProductSendOwner::State::kIdle);
  p.record_present = true;
  sender.poll(30000U);
  sender.poll(30001U);
  assert(p.tx == 1);
  p.record_present = false;
  sender.poll(60001U);
  assert(p.tx == 1 && sender.state() == ProductSendOwner::State::kFault);
}

void testWraparoundDeadline() {
  Fake p;
  ProductSendOwner sender(p);
  sender.poll(0xFFFFFFF0U);
  sender.poll(0xFFFFFFF1U);
  assert(p.tx == 1);
  sender.poll(29984U);
  assert(p.tx == 1);
  sender.poll(29985U);
  assert(p.tx == 2 && p.seals == 1);
}

}  // namespace

int main() {
  testStoreBeforeSendAndBackoff();
  testBackoffReachesFifteenMinuteCap();
  testCachedObjectChangesBetweenReads();
  testSealUnavailableThrottlesFlashScan();
  testRebootUsesExactAndBurnsNoCounter();
  testSecurityAndNoCredentialLifetime();
  testWriteFailuresFailClosed();
  testUnavailableRadioAndInvalidatedCache();
  testNoRecordAndReleasedBacking();
  testWraparoundDeadline();
  puts("SF5J2B1 dormant product send coordinator: PASS");
}
