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
  uint32_t report_period_ms = 60000U;
  uint32_t record_sequence = 7U;
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
    out.identity.sequence = record_sequence;
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
  uint32_t reportPeriodMs() const override { return report_period_ms; }
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
  assert(p.tx == 2);  // no repeated 60/120/... retry timer
  sender.onReportCommitted();
  sender.poll(90002U);
  assert(p.tx == 3 && p.seals == 1 && p.puts == 1);
}


void testOneHalfPeriodRetryThenReportOnly() {
  for (const uint32_t report_period : {3600000U, 7200000U}) {
    Fake p;
    p.report_period_ms = report_period;
    ProductSendOwner sender(p);
    sender.poll(0U);  // no TX before the durable cache outcome
    sender.poll(1U);  // initial exact-object probe
    assert(p.tx == 1U && p.seals == 1U && p.puts == 1U);
    const uint32_t half = report_period / 2U;
    sender.poll(half);
    assert(p.tx == 1U);
    sender.poll(half + 1U);
    assert(p.tx == 2U);
    assert(sender.state() == ProductSendOwner::State::kWaitReport);
    // Even hours of silence do not start more arbitrary retries.
    for (uint32_t t = half + 2U; t < 4U * report_period; t += 51011U)
      sender.poll(t);
    assert(p.tx == 2U && p.seals == 1U && p.puts == 1U);
    // A newly COMMITTED normal report arms one contact opportunity, using
    // the old exact ciphertext as probe; never a fifth cached object.
    sender.onReportCommitted();
    sender.poll(4U * report_period);
    assert(p.tx == 3U && p.seals == 1U && p.puts == 1U);
    sender.poll(6U * report_period);
    assert(p.tx == 3U);
    sender.onReportCommitted();
    sender.poll(6U * report_period + 1U);
    assert(p.tx == 4U && p.seals == 1U && p.puts == 1U);
  }
}

void testOnlyCommittedCustodyReleaseResumesBacklog() {
  Fake p;
  ProductSendOwner sender(p);
  sender.poll(0U);
  sender.poll(1U);
  assert(p.tx == 1U);
  ProductSendIdentity wrong{};
  wrong.incarnation = 42U;
  wrong.sequence = 999U;
  sender.onCustodyReleaseCommitted(wrong, 10U);
  assert(sender.state() == ProductSendOwner::State::kRetry);
  // Simulate a separately authenticated receipt + durable store release.
  // The sender itself neither trusts RF_RX nor mutates ObservationStore.
  p.record_sequence = 8U;
  p.cached_present = false;
  ProductSendIdentity committed{};
  committed.incarnation = 42U;
  committed.sequence = 7U;
  sender.onCustodyReleaseCommitted(committed, 100U);
  assert(sender.state() == ProductSendOwner::State::kIdle);
  sender.poll(15099U);
  assert(p.tx == 1U && p.puts == 1U);
  sender.poll(15100U);
  assert(p.puts == 2U && p.tx == 1U);
  sender.poll(15101U);
  assert(p.tx == 2U && p.seals == 2U);
}

void testSixReportsOneProbeAndSelectiveBacklogDrain() {
  Fake p;
  p.record_sequence = 1U;
  p.report_period_ms = 3600000U;
  ProductSendOwner sender(p);
  sender.poll(0U);    // First PERIODIC sealed and cached
  sender.poll(1U);    // Contact attempt
  sender.poll(1800001U); // Only extra half-hour retry
  assert(p.tx == 2U && p.puts == 1U && p.seals == 1U);
  for (uint32_t period = 2U; period <= 6U; ++period) {
    // New logical periods are stored by the independent observation owner.
    // Unacknowledged encrypted object 1 remains our on-air contact probe.
    sender.onReportCommitted();
    sender.poll(period * 3600000U);
    assert(p.tx == period + 1U);
    assert(p.puts == 1U && p.seals == 1U);
    if (period < 6U) {
      sender.poll(period * 3600000U + 1799000U);
      assert(p.tx == period + 1U);
    }
  }
  // The ACK at the sixth period was for exact object 1, NOT automatic
  // acceptance of records 2..6. Simulate separately verified, durably
  // released custody for each subsequent oldest record.
  for (uint32_t delivered = 1U; delivered <= 5U; ++delivered) {
    ProductSendIdentity acknowledged{};
    acknowledged.incarnation = 42U;
    acknowledged.sequence = delivered;
    p.record_sequence = delivered + 1U;
    p.cached_present = false;
    const uint32_t accepted_at = 6U * 3600000U + delivered * 20000U;
    sender.onCustodyReleaseCommitted(acknowledged, accepted_at);
    sender.poll(accepted_at + 14999U);
    assert(p.tx == 6U + delivered);
    sender.poll(accepted_at + 15000U);
    assert(p.puts == delivered + 1U);
    sender.poll(accepted_at + 15001U);
    assert(p.tx == 7U + delivered);
  }
  assert(p.seals == 6U && p.puts == 6U);
  assert(sender.state() == ProductSendOwner::State::kRetry);
  assert(sender.diagnostics().tx_started == p.tx);
}

void testMissingCadenceDisablesExtraRetry() {
  Fake p;
  p.report_period_ms = 0U;
  ProductSendOwner sender(p);
  sender.poll(0U);
  sender.poll(1U);
  assert(p.tx == 1U && sender.state() == ProductSendOwner::State::kWaitReport);
  sender.poll(10000000U);
  assert(p.tx == 1U);
  sender.onReportCommitted();
  sender.poll(10000001U);
  assert(p.tx == 2U && p.seals == 1U);
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
  sender.onReportCommitted();
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
  testOneHalfPeriodRetryThenReportOnly();
  testOnlyCommittedCustodyReleaseResumesBacklog();
  testSixReportsOneProbeAndSelectiveBacklogDrain();
  testMissingCadenceDisablesExtraRetry();
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
