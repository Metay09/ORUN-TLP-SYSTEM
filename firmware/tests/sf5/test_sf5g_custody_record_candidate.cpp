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
  // Don't authorize this state as custody ACK: no such interface exists.
}
}
int main() {
  checkOne(84);
  checkOne(100);
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
