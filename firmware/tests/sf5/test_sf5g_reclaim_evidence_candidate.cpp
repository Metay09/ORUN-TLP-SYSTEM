#include "sf5g_reclaim_evidence_candidate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>

using namespace orun_tlp::sf5g_reclaim_evidence_candidate;

namespace {
std::vector<uint8_t> makeFrame(size_t n, uint8_t seed) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0U; i < n; ++i)
    bytes[i] = static_cast<uint8_t>(13U * i + seed);
  bytes[0] = 2U;
  bytes[1] = 4U;
  bytes[2] = 2U;
  bytes[3] = n == 100U ? 1U : 2U;
  bytes[4] = 2U;
  bytes[5] = 1U;
  bytes[6] = n == 100U ? 64U : 48U;
  bytes[7] = 0U;
  assert(record::productObjectShape(bytes.data(), bytes.size()));
  return bytes;
}

class FakeClaims final : public ReceiptSource {
 public:
  uint16_t page_index = 1U;
  uint64_t generation = 7U;
  std::array<std::vector<uint8_t>, format::kRecordsPerPage> expected{};
  std::array<bool, format::kRecordsPerPage> accepted{};
  uint16_t invalid_slot = UINT16_MAX;
  mutable size_t calls = 0U;

  Claim lookup(uint16_t physical_page, uint64_t gen, uint16_t slot,
               const uint8_t* exact, size_t size) const override {
    ++calls;
    if (slot >= expected.size() || physical_page != page_index ||
        gen != generation || expected[slot].size() != size ||
        exact == nullptr ||
        memcmp(expected[slot].data(), exact, size) != 0)
      return Claim::kAbsent;
    if (slot == invalid_slot) return Claim::kInvalid;
    return accepted[slot] ? Claim::kClaimsDurableExactObject
                          : Claim::kAbsent;
  }
};

class Fixture {
 public:
  Fixture() {
    nor::PageWriter writer(media);
    assert(writer.initializeEmpty(7U, true) == nor::WriteResult::kInitialized);
    for (size_t i = 0U; i < format::kRecordsPerPage; ++i) {
      auto frame = makeFrame(i % 2U == 0U ? 84U : 100U,
                             static_cast<uint8_t>(17U + i));
      claims.expected[i] = frame;
      assert(writer.appendExact(frame.data(), frame.size()) ==
             nor::WriteResult::kStored);
    }
  }
  void handoff(size_t slot, bool second = false) {
    assert(slot < format::kRecordsPerPage);
    const uint8_t zeros[4] = {0U, 0U, 0U, 0U};
    const size_t off = format::kHeaderSize + slot * format::kRecordSize +
                       (second ? record::kRecordHandoff1Offset
                               : record::kRecordHandoff0Offset);
    assert(media.program(off, zeros, sizeof(zeros)));
  }
  void handoffAll() {
    for (size_t slot = 0U; slot < format::kRecordsPerPage; ++slot)
      handoff(slot);
  }
  void acceptAll() {
    claims.accepted.fill(true);
  }
  Review run() { return reviewPage(media, 1U, 4U, claims); }

  nor::NorPage media;
  FakeClaims claims;
};

void testHeldAndMissingEvidence() {
  Fixture f;
  f.acceptAll(); // Mock claims do NOT overrule on-flash HELD records.
  Review r = f.run();
  assert(r.state == ReviewState::kHeld && r.first_blocked_slot == 0U);
  assert(f.claims.calls == 0U);
  for (size_t i = 0U; i < format::kRecordsPerPage - 1U; ++i)
    f.handoff(i);
  r = f.run();
  assert(r.state == ReviewState::kHeld);
  assert(r.first_blocked_slot == format::kRecordsPerPage - 1U);
  assert(f.claims.calls == 0U);

  f.handoff(format::kRecordsPerPage - 1U);
  f.claims.accepted.fill(false);
  r = f.run();
  assert(r.state == ReviewState::kMissingReceipt);
  assert(r.first_blocked_slot == 0U && r.checked == 1U);

  f.acceptAll();
  r = f.run();
  assert(r.state == ReviewState::kCandidateOnly);
  assert(r.checked == format::kRecordsPerPage);
  assert(r.generation == 7U && r.first_blocked_slot == UINT16_MAX);
  // "CandidateOnly" is a classification, NEVER an erase capability.
}

void testBindingAndErrors() {
  Fixture f;
  f.handoffAll();
  f.acceptAll();
  f.claims.accepted[18U] = false;
  Review r = f.run();
  assert(r.state == ReviewState::kMissingReceipt);
  assert(r.first_blocked_slot == 18U && r.checked == 19U);

  f.acceptAll();
  f.claims.generation = 8U;  // stale or wrong generation
  assert(f.run().state == ReviewState::kMissingReceipt);
  f.claims.generation = 7U;
  f.claims.page_index = 2U;
  assert(f.run().state == ReviewState::kMissingReceipt);
  f.claims.page_index = 1U;
  f.claims.expected[9U][30U] ^= 1U; // different exact sealed ciphertext
  r = f.run();
  assert(r.state == ReviewState::kMissingReceipt);
  assert(r.first_blocked_slot == 9U);
  f.claims.expected[9U][30U] ^= 1U;
  f.claims.invalid_slot = 4U;
  r = f.run();
  assert(r.state == ReviewState::kRefused && r.first_blocked_slot == 4U);
  f.claims.invalid_slot = UINT16_MAX;
  assert(f.run().state == ReviewState::kCandidateOnly);

  f.claims.calls = 0U;
  assert(reviewPage(f.media, 4U, 4U, f.claims).state ==
         ReviewState::kRefused);
  assert(reviewPage(f.media, 1U, 0U, f.claims).state ==
         ReviewState::kRefused);
  assert(reviewPage(f.media, 1U, 33U, f.claims).state ==
         ReviewState::kRefused);
  assert(f.claims.calls == 0U);
  f.media.setReadBudget(0U);
  assert(f.run().state == ReviewState::kReadFailure);
  assert(f.claims.calls == 0U);
}

void testNotFullAndForeign() {
  nor::NorPage empty;
  FakeClaims claims;
  assert(reviewPage(empty, 0U, 4U, claims).state ==
         ReviewState::kRefused);  // no ACTIVE OCP3 header
  nor::PageWriter writer(empty);
  assert(writer.initializeEmpty(3U, true) == nor::WriteResult::kInitialized);
  Review r = reviewPage(empty, 0U, 4U, claims);
  assert(r.state == ReviewState::kNotFull && claims.calls == 0U);
  const auto a = makeFrame(84U, 75U);
  assert(writer.appendExact(a.data(), a.size()) == nor::WriteResult::kStored);
  assert(reviewPage(empty, 0U, 4U, claims).state ==
         ReviewState::kNotFull);

  nor::NorPage legacy;
  const uint8_t old_magic[] = {'O', 'C', 'Q', '1'};
  assert(legacy.program(0U, old_magic, sizeof(old_magic)));
  assert(reviewPage(legacy, 0U, 4U, claims).state ==
         ReviewState::kRefused);
}

void testRejectAmbiguousBytes() {
  {
    Fixture f;
    f.handoffAll();
    f.acceptAll();
    const uint8_t partial = 0xFEU;
    const size_t at = format::kHeaderSize + 5U * format::kRecordSize +
                      record::kRecordHandoff1Offset;
    assert(f.media.program(at, &partial, 1U));
    assert(f.run().state == ReviewState::kRefused);
  }
  {
    Fixture f;
    f.handoffAll();
    f.acceptAll();
    // A-only handoff ordering is mandatory: B committed with A erased
    // is impossible for an ordered writer and must not count as release.
    const uint8_t ones[4] = {0U, 0U, 0U, 0U};
    // First construct a fresh page with B-only on one slot.
    Fixture broken;
    broken.acceptAll();
    for (size_t i = 0U; i < format::kRecordsPerPage; ++i) {
      if (i == 2U)
        broken.handoff(i, true);
      else
        broken.handoff(i);
    }
    (void)ones;
    assert(broken.run().state == ReviewState::kRefused);
  }
  {
    Fixture f;
    f.handoffAll();
    f.acceptAll();
    // Corrupt a non-header payload byte; crc readback must now disagree.
    const uint8_t zero = 0U;
    const size_t at = format::kHeaderSize + 2U * format::kRecordSize +
                      record::kRecordObjectOffset + 30U;
    assert(f.media.program(at, &zero, 1U));
    assert(f.run().state == ReviewState::kRefused);
  }
  {
    Fixture f;
    f.handoffAll();
    f.acceptAll();
    // The reclaimed-intent area must not be active/pending in this
    // isolated advisory review; even a single unknown byte blocks it.
    const uint8_t touched = 0xFEU;
    assert(f.media.program(format::kPageSize - 1U, &touched, 1U));
    assert(f.run().state == ReviewState::kRefused);
  }
}

}  // namespace

int main() {
  testHeldAndMissingEvidence();
  testBindingAndErrors();
  testNotFullAndForeign();
  testRejectAmbiguousBytes();
  puts("SF5G reclaim evidence review: held/exact-binding/foreign/torn/no-erase: PASS");
}
