// KQ05 HOST ONLY: seven pages of synthetic NOR; absolutely no device Flash.
// This test DOES NOT activate or format a supplemental storage writer.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_supplemental_28k_census.h"

namespace sub = orun_tlp::kq05_supplemental;
namespace obs = orun_tlp::observation_store_format;
namespace cust = orun_tlp::custody_store_product_format;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;

namespace {

constexpr uint64_t kTrackerId = UINT64_C(0x1020304050607080);
constexpr uint64_t kGatewayId = UINT64_C(0x4757415949443031);

struct FakeHistory final : FlashBackend {
  uint8_t bytes[sub::kBytes]{};
  mutable uint32_t reads = 0;
  uint32_t begins = 0;
  uint32_t writes = 0;
  uint32_t erases = 0;
  int32_t failed_read_page = -1;
  FakeHistory() { memset(bytes, 0xFF, sizeof(bytes)); }

  bool begin() override { ++begins; return false; }  // must never call
  bool read(uint32_t offset, void* to, size_t length) const override {
    ++reads;
    if (to == nullptr || offset > sub::kBytes ||
        length > sub::kBytes - offset ||
        (failed_read_page >= 0 &&
         offset / sub::kPageBytes ==
           static_cast<uint32_t>(failed_read_page))) return false;
    memcpy(to, bytes + offset, length);
    return true;
  }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++writes;
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erases;
    return FlashOpResult::kFailed;
  }

  void trackerPage(uint32_t page, uint64_t device = kTrackerId) {
    assert(page < sub::kPages);
    obs::encodePageHeader(obs::PageKind::kData, page + 1U,
                          device, 1U, bytes + page * sub::kPageBytes);
  }
  void gatewayPage(uint32_t page) {
    assert(page < sub::kPages);
    uint8_t header[cust::kPageHeaderSize]{};
    assert(cust::encodePageHeader(page + 1U, header));
    memcpy(bytes + page * sub::kPageBytes, header, sizeof(header));
  }
  void gatewayBoundPage(uint32_t page, uint64_t gateway = kGatewayId) {
    assert(page < sub::kPages);
    uint8_t header[cust::kPageHeaderSize]{};
    assert(cust::encodeBoundPageHeader(page + 1U, gateway, header));
    memcpy(bytes + page * sub::kPageBytes, header, sizeof(header));
  }
  void assertNoMutation() const {
    assert(begins == 0U && writes == 0U && erases == 0U);
  }
};

void testAllBlankAndCapacity() {
  static_assert(sub::kBytes == 28672U, "28 KiB expected");
  static_assert(sub::kTrackerNominalEntries == 210U, "five OBS1 data pages");
  static_assert(sub::kGatewayNominalEntries == 231U, "seven OCQ2 pages");
  FakeHistory fake;
  const sub::Result a = sub::inspectReadOnly(
      fake, sub::Role::kTracker, kTrackerId);
  assert(a.state == sub::State::kBlank);
  assert(a.erased_pages == 7U && a.candidate_pages == 0U);
  assert(fake.reads == 7U * 64U);
  const sub::Result b = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId);
  assert(b.state == sub::State::kBlank);
  assert(b.erased_pages == 7U);
  fake.assertNoMutation();
}

void testRoleBlockedWithoutReading() {
  FakeHistory fake;
  const sub::Role disallowed[2] = {
      sub::Role::kUnassigned, sub::Role::kConflict};
  for (size_t i = 0U; i < 2U; ++i) {
    const sub::Result out = sub::inspectReadOnly(
        fake, disallowed[i], kTrackerId);
    assert(out.state == sub::State::kBlockedRole);
  }
  assert(sub::inspectReadOnly(fake, sub::Role::kTracker, 0U).state ==
         sub::State::kBlockedRole);
  assert(sub::inspectReadOnly(fake, sub::Role::kGateway, UINT64_MAX).state ==
         sub::State::kBlockedRole);
  assert(fake.reads == 0U);
  fake.assertNoMutation();
}

void testTrackerOwnsOnlyTrackerData() {
  FakeHistory fake;
  fake.trackerPage(0U);
  fake.trackerPage(6U);
  const sub::Result ok = sub::inspectReadOnly(
      fake, sub::Role::kTracker, kTrackerId);
  assert(ok.state == sub::State::kPotentialTracker);
  assert(ok.candidate_pages == 2U && ok.erased_pages == 5U);
  assert(sub::inspectReadOnly(fake, sub::Role::kGateway, kGatewayId).state ==
         sub::State::kLegacyOrForeign);

  const sub::Result wrong_id = sub::inspectReadOnly(
      fake, sub::Role::kTracker, kTrackerId + 1U);
  assert(wrong_id.state == sub::State::kLegacyOrForeign);
  assert(wrong_id.first_bad_page == 0U);
  fake.assertNoMutation();
}

void testGatewayCannotMountUnbound() {
  FakeHistory fake;
  fake.gatewayPage(0U);
  fake.gatewayPage(3U);
  const sub::Result r = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId);
  assert(r.state == sub::State::kGatewayNeedsBinding);
  assert(r.candidate_pages == 2U);
  assert(r.erased_pages == 5U);
  // The same OCQ2 bytes with a DIFFERENT Gateway ID still must never be
  // classified "mounted"; OCQ2 headers contain no per-device binding.
  const sub::Result foreign = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId + 1U);
  assert(foreign.state == sub::State::kGatewayNeedsBinding);
  assert(sub::inspectReadOnly(fake, sub::Role::kTracker, kTrackerId).state ==
         sub::State::kLegacyOrForeign);
  fake.assertNoMutation();
}

void testGatewayBoundToExactLocalId() {
  FakeHistory fake;
  fake.gatewayBoundPage(0U);
  fake.gatewayBoundPage(5U);
  const sub::Result correct = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId);
  assert(correct.state == sub::State::kGatewayBoundCandidate);
  assert(correct.candidate_pages == 2U && correct.erased_pages == 5U);
  const sub::Result other = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId + 1U);
  assert(other.state == sub::State::kLegacyOrForeign);
  assert(other.first_bad_page == 0U);

  // Mixing previously unbound v3 custody and the new v4 bound pages is
  // never an implicit migration permission.
  fake.gatewayPage(6U);
  const sub::Result mixed = sub::inspectReadOnly(
      fake, sub::Role::kGateway, kGatewayId);
  assert(mixed.state == sub::State::kLegacyOrForeign);
  assert(mixed.first_bad_page == 6U);
  fake.assertNoMutation();
}

void testLegacyOrPartialPageRejected() {
  FakeHistory old;
  old.bytes[0] = 'H';
  old.bytes[1] = 'I';
  old.bytes[2] = 'S';
  old.bytes[3] = 'T';
  assert(sub::inspectReadOnly(old, sub::Role::kGateway, kGatewayId).state ==
         sub::State::kLegacyOrForeign);

  FakeHistory partial;
  partial.bytes[6U * sub::kPageBytes + 1024U] = 0U;
  const sub::Result res = sub::inspectReadOnly(
      partial, sub::Role::kTracker, kTrackerId);
  assert(res.state == sub::State::kLegacyOrForeign);
  assert(res.first_bad_page == 6U);

  FakeHistory torn;
  torn.trackerPage(0U);
  torn.bytes[obs::kPageHeaderCommitOffset] = 0xFFU;
  const sub::Result invalid = sub::inspectReadOnly(
      torn, sub::Role::kTracker, kTrackerId);
  assert(invalid.state == sub::State::kCorruptOrIncomplete);
  assert(invalid.first_bad_page == 0U);
  old.assertNoMutation();
  partial.assertNoMutation();
  torn.assertNoMutation();
}

void testReadFailureRefusesScan() {
  FakeHistory fake;
  fake.failed_read_page = 2;
  const sub::Result out = sub::inspectReadOnly(
      fake, sub::Role::kTracker, kTrackerId);
  assert(out.state == sub::State::kReadFailed);
  assert(out.first_bad_page == 2U);
  fake.assertNoMutation();
}

}  // namespace

int main() {
  testAllBlankAndCapacity();
  testRoleBlockedWithoutReading();
  testTrackerOwnsOnlyTrackerData();
  testGatewayCannotMountUnbound();
  testGatewayBoundToExactLocalId();
  testLegacyOrPartialPageRejected();
  testReadFailureRefusesScan();
  puts("KQ05 28KiB supplemental role-aware read-only census: PASS (HOST ONLY)");
  puts("NO DEVICE FLASH, NO ERASE, NO MIGRATION, NO ACK");
  return 0;
}
