#include "sf5g_custody_record_candidate.h"
#include "sf5b_product_vectors.h"  // independent SF5B sealed golden frames

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>

using namespace orun_tlp::sf5g_custody_candidate;

namespace {
std::vector<uint8_t> fakeFrame(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 13U + 7U);
  v[0] = 2; v[1] = 4; v[2] = 2; v[3] = n == 100 ? 1 : 2;
  v[4] = 3; v[5] = 1; v[6] = n == 100 ? 64 : 48; v[7] = 0;
  return v;
}
void checkOne(size_t n) {
  const std::vector<uint8_t> frame = fakeFrame(n);
  assert(productObjectShape(frame.data(), frame.size()));
  std::array<uint8_t, kRecordSize> raw{};
  raw.fill(0xA5);
  assert(encodeStagedRecord(frame.data(), frame.size(), raw.data(), raw.size()));
  RecordInspection found;
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kStaged);
  // Every partial prefix tear before separately programming commit must fail closed.
  const auto staged = raw;
  for (size_t prefix = 0; prefix < kRecordCommitOffset; ++prefix) {
    auto torn = std::array<uint8_t,kRecordSize>{};
    torn.fill(0xFF);
    memcpy(torn.data(), staged.data(), prefix);
    assert(inspectRecord(torn.data(), torn.size(), found));
    assert(found.evidence != Evidence::kHeld && found.evidence != Evidence::kHandedOff);
  }
  raw[kRecordCommitOffset] = 0xFE;
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kPartialCommit);
  // Proper commit: after verified complete data + CRC, only then zeros.
  raw = staged;
  memset(raw.data() + kRecordCommitOffset, 0, 4);
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kHeld);
  assert(found.object_size == n);
  assert(sameExactObject(found, frame.data(), frame.size()));
  std::vector<uint8_t> changed = frame;
  changed.back() ^= 0x80;
  assert(!sameExactObject(found, changed.data(), changed.size()));
  // Simulated reset: the record bytes alone recover the same exact object.
  RecordInspection remounted;
  assert(inspectRecord(raw.data(), raw.size(), remounted));
  assert(sameExactObject(remounted, frame.data(), frame.size()));
  auto corrupted = raw;
  corrupted[kRecordObjectOffset + n - 1] ^= 1;
  assert(inspectRecord(corrupted.data(), corrupted.size(), found));
  assert(found.evidence == Evidence::kCorrupt);
  if (n == 84) {
    corrupted = raw;
    corrupted[kRecordObjectOffset + n] = 0;
    assert(inspectRecord(corrupted.data(), corrupted.size(), found));
    assert(found.evidence == Evidence::kCorrupt);
  }
  corrupted = raw;
  corrupted[kRecordLengthOffset] ^= 1;
  assert(inspectRecord(corrupted.data(), corrupted.size(), found));
  assert(found.evidence == Evidence::kCorrupt);
  // Torn handoff marker retains uncertain HELD state; second slot permits progress.
  raw[kRecordHandoff0Offset] = 0xFE;
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kHeld && found.handoff_uncertain);
  assert(found.next_handoff_slot == 1);
  memset(raw.data() + kRecordHandoff1Offset, 0, 4);
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kHandedOff);
  assert(sameExactObject(found, frame.data(), frame.size()));
  // When BOTH marker slots have torn, retain the exact object but do not
  // mistake either partial marker for completed handoff or free space.
  raw = staged;
  memset(raw.data() + kRecordCommitOffset, 0, 4);
  raw[kRecordHandoff0Offset] = 0xFE;
  raw[kRecordHandoff1Offset] = 0xFE;
  assert(inspectRecord(raw.data(), raw.size(), found));
  assert(found.evidence == Evidence::kHeld);
  assert(found.handoff_uncertain && found.next_handoff_slot == 0xFFU);
  assert(sameExactObject(found, frame.data(), frame.size()));
  // Don't authorize this state as custody ACK: no such interface exists.
}

// Independent admission matrix for both supported PRODUCT_SECURE families.
// Only custody-requested path flags are eligible; RELAY_ALLOWED is optional.
void testCustodyPathFlags() {
  for (size_t n : {84U, 100U}) {
    for (unsigned flags = 0U; flags < 8U; ++flags) {
      std::vector<uint8_t> frame = fakeFrame(n);
      frame[4] = static_cast<uint8_t>(flags);
      const bool accept = (flags == 2U || flags == 3U);
      assert(productObjectShape(frame.data(), frame.size()) == accept);
      std::array<uint8_t, kRecordSize> staged{};
      staged.fill(0x7AU);
      assert(encodeStagedRecord(frame.data(), frame.size(),
                                staged.data(), staged.size()) == accept);
      if (!accept) {
        assert(staged[0] == 0x7AU);
        continue;
      }
      memset(staged.data() + kRecordCommitOffset, 0, 4U);
      RecordInspection found;
      assert(inspectRecord(staged.data(), staged.size(), found));
      assert(found.evidence == Evidence::kHeld);
      assert(sameExactObject(found, frame.data(), frame.size()));
    }
  }
}

void testDelegatedRejectedByFamilyNotJustLength() {
  for (size_t n : {84U, 96U, 100U}) {
    std::vector<uint8_t> delegated = fakeFrame(n);
    delegated[1] = 0x01U;  // DELEGATED_SECURE_APP
    delegated[2] = 0x04U;  // D2GW
    delegated[3] = 0x02U;  // RESULT
    assert(!productObjectShape(delegated.data(), delegated.size()));
    std::array<uint8_t, kRecordSize> out{};
    out.fill(0x7AU);
    assert(!encodeStagedRecord(delegated.data(), delegated.size(),
                               out.data(), out.size()));
    assert(out[0] == 0x7AU);
  }
}

void testBitTearsAndNegativeStates() {
  std::vector<uint8_t> frame = fakeFrame(100U);
  std::array<uint8_t, kRecordSize> staged{};
  assert(encodeStagedRecord(frame.data(), frame.size(),
                            staged.data(), staged.size()));
  std::array<uint8_t, kRecordSize> committed = staged;
  memset(committed.data() + kRecordCommitOffset, 0, 4U);
  RecordInspection state;
  assert(inspectRecord(committed.data(), committed.size(), state));
  assert(state.evidence == Evidence::kHeld);

  // Flip each one of the 864 data/CRC bits of an otherwise committed record.
  // A torn body or CRC must never become HELD or HANDED_OFF.
  for (size_t at = 0U; at < kRecordCommitOffset; ++at) {
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      auto damaged = committed;
      damaged[at] ^= static_cast<uint8_t>(1U << bit);
      assert(inspectRecord(damaged.data(), damaged.size(), state));
      assert(state.evidence != Evidence::kHeld &&
             state.evidence != Evidence::kHandedOff);
    }
  }
  // Each nonzero commit byte (all 255 byte values, at all 4 offsets)
  // means PARTIAL_COMMIT, regardless of the perfect data and CRC.
  for (size_t at = kRecordCommitOffset; at < kRecordCommitOffset + 4U; ++at) {
    for (unsigned value = 1U; value <= 255U; ++value) {
      auto damaged = committed;
      damaged[at] = static_cast<uint8_t>(value);
      assert(inspectRecord(damaged.data(), damaged.size(), state));
      assert(state.evidence == Evidence::kPartialCommit);
    }
  }
  // Commit absent, handoff programmed: the handoff must not promote a
  // record that was never committed.
  auto abnormal = staged;
  memset(abnormal.data() + kRecordHandoff0Offset, 0, 4U);
  assert(inspectRecord(abnormal.data(), abnormal.size(), state));
  assert(state.evidence == Evidence::kStaged);

  // Completely erased data/CRC plus committed marker has no valid object.
  abnormal.fill(0xFFU);
  memset(abnormal.data() + kRecordCommitOffset, 0, 4U);
  assert(inspectRecord(abnormal.data(), abnormal.size(), state));
  assert(state.evidence == Evidence::kCorrupt);

  // A legacy 73-byte item is never a valid product frame, even when the
  // record CRC is recalculated correctly for that unsupported length.
  abnormal = committed;
  abnormal[0] = 0U; abnormal[1] = 73U;
  const uint32_t legacy_crc = crc32(abnormal.data(), kRecordCrcOffset);
  abnormal[kRecordCrcOffset] = static_cast<uint8_t>(legacy_crc >> 24);
  abnormal[kRecordCrcOffset + 1U] = static_cast<uint8_t>(legacy_crc >> 16);
  abnormal[kRecordCrcOffset + 2U] = static_cast<uint8_t>(legacy_crc >> 8);
  abnormal[kRecordCrcOffset + 3U] = static_cast<uint8_t>(legacy_crc);
  assert(inspectRecord(abnormal.data(), abnormal.size(), state));
  assert(state.evidence == Evidence::kCorrupt);

  // Impossible handoff order: A erased, B torn. Do not offer A as writable.
  abnormal = committed;
  abnormal[kRecordHandoff1Offset] = 0xFEU;
  assert(inspectRecord(abnormal.data(), abnormal.size(), state));
  assert(state.evidence == Evidence::kHeld);
  assert(state.handoff_uncertain && state.next_handoff_slot == 0xFFU);
  assert(sameExactObject(state, frame.data(), frame.size()));
}

void testCrossLanguageCrcVectors() {
  const uint8_t kat[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert(crc32(kat, sizeof(kat)) == 0xCBF43926U);
  struct Vector { const char* hex; size_t size; uint32_t crc; };
  const Vector cases[] = {
      {orun_tlp::sf5b_vectors::kPeriodicFrameHex, 100U, 0x8F3C43A4U},
      {orun_tlp::sf5b_vectors::kEventFrameHex, 84U, 0x2BC3C99CU},
  };
  for (const Vector& v : cases) {
    std::vector<uint8_t> frame(v.size);
    assert(orun_tlp::sf5b_vectors::fromHex(v.hex, frame.data(), v.size));
    std::array<uint8_t, kRecordSize> staged{};
    assert(encodeStagedRecord(frame.data(), frame.size(),
                              staged.data(), staged.size()));
    assert(crc32(staged.data(), kRecordCrcOffset) == v.crc);
    assert(staged[kRecordCrcOffset] == static_cast<uint8_t>(v.crc >> 24));
    assert(staged[kRecordCrcOffset + 1U] ==
           static_cast<uint8_t>(v.crc >> 16));
    assert(staged[kRecordCrcOffset + 2U] ==
           static_cast<uint8_t>(v.crc >> 8));
    assert(staged[kRecordCrcOffset + 3U] == static_cast<uint8_t>(v.crc));
  }
}
}
int main() {
  checkOne(84);
  checkOne(100);
  testCustodyPathFlags();
  testDelegatedRejectedByFamilyNotJustLength();
  testBitTearsAndNegativeStates();
  testCrossLanguageCrcVectors();
  // Exact encrypted PRODUCT_SECURE bytes from the independent SF5B golden set.
  for (const char* hex : {orun_tlp::sf5b_vectors::kEventFrameHex,
                          orun_tlp::sf5b_vectors::kPeriodicFrameHex}) {
    const size_t n = strlen(hex) / 2U;
    std::vector<uint8_t> frame(n);
    assert(orun_tlp::sf5b_vectors::fromHex(hex, frame.data(), n));
    std::array<uint8_t,kRecordSize> slot{};
    assert(encodeStagedRecord(frame.data(), frame.size(),
                              slot.data(), slot.size()));
    memset(slot.data() + kRecordCommitOffset, 0, 4);
    RecordInspection actual;
    assert(inspectRecord(slot.data(), slot.size(), actual));
    assert(actual.evidence == Evidence::kHeld);
    assert(sameExactObject(actual, frame.data(), frame.size()));
  }
  // The comparison API must reject an externally corrupted length rather
  // than reading past RecordInspection.object[100] under ASan.
  RecordInspection forged;
  forged.evidence = Evidence::kHeld;
  forged.object_size = 101U;
  std::vector<uint8_t> oversized(101U, 0xAAU);
  assert(!sameExactObject(forged, oversized.data(), oversized.size()));
  forged.object_size = 65535U;
  std::vector<uint8_t> huge(65535U, 0xAAU);
  assert(!sameExactObject(forged, huge.data(), huge.size()));
  forged.object_size = 101U;
  std::vector<uint8_t> correct_size(100U, 0xAAU);
  memcpy(forged.object, correct_size.data(), correct_size.size());
  assert(!sameExactObject(forged, correct_size.data(), correct_size.size()));
  // Positive control proves the negative evidence/size cases fail due
  // to their gate, NOT because memcmp happens to differ.
  forged.object_size = 100U;
  assert(sameExactObject(forged, correct_size.data(), correct_size.size()));
  for (Evidence invalid : {Evidence::kCorrupt, Evidence::kStaged,
                           Evidence::kPartialCommit, Evidence::kErased}) {
    forged.evidence = invalid;
    assert(!sameExactObject(forged, correct_size.data(),
                            correct_size.size()));
  }
  forged.evidence = Evidence::kHeld;
  for (size_t invalid_size : {50U, 99U}) {
    forged.object_size = static_cast<uint16_t>(invalid_size);
    std::vector<uint8_t> short_object(invalid_size, 0xAAU);
    assert(!sameExactObject(forged, short_object.data(),
                            short_object.size()));
  }
  auto p = fakeFrame(100);
  std::array<uint8_t,kRecordSize> out{};
  out.fill(0x7A);
  // A custody-eligible DELEGATED_D2GW RESULT can be 96 bytes, but is not
  // authorized by this SF5G candidate's PRODUCT_SECURE-only discriminator.
  // Its separate type/context/family and admission policy remain a gate.
  auto delegated_result = p;
  delegated_result.resize(96);
  delegated_result[1] = 0x01;  // DELEGATED_SECURE_APP
  delegated_result[2] = 0x04;  // delegated D2GW
  delegated_result[3] = 0x02;  // RESULT
  assert(!productObjectShape(delegated_result.data(), delegated_result.size()));
  assert(!encodeStagedRecord(delegated_result.data(), delegated_result.size(),
                             out.data(), out.size()));
  assert(out[0] == 0x7A);
  for (unsigned offset: {0U,1U,2U,3U,4U,5U,6U,7U}) {
    auto bad = p;
    bad[offset] = 0xFF;
    assert(!encodeStagedRecord(bad.data(), bad.size(), out.data(), out.size()));
    assert(out[0] == 0x7A);  // no mutation on reject
  }
  auto zero_counter = p;
  memset(zero_counter.data() + 20, 0, 8);
  assert(!encodeStagedRecord(zero_counter.data(), zero_counter.size(),
                             out.data(), out.size()));
  auto invalid_epoch = p;
  memset(invalid_epoch.data() + 16, 0xFF, 4);
  assert(!encodeStagedRecord(invalid_epoch.data(), invalid_epoch.size(),
                             out.data(), out.size()));
  assert(!encodeStagedRecord(p.data(), 99, out.data(), out.size()));
  RecordInspection found;
  assert(!inspectRecord(nullptr, kRecordSize, found));
  assert(!inspectRecord(out.data(), 7, found));
  printf("SF5G portable record candidate: staged/torn/reboot/CRC/handoff checks: PASS\n");
}
