#include "sf5g_nor_page_writer_candidate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <initializer_list>
#include <vector>

using namespace orun_tlp::sf5g_nor_writer_candidate;

namespace {
std::vector<uint8_t> frame(size_t n, uint8_t seed = 11U) {
  std::vector<uint8_t> data(n);
  for (size_t i = 0; i < n; ++i)
    data[i] = static_cast<uint8_t>(i * 13U + seed);
  data[0] = 2U; data[1] = 4U; data[2] = 2U;
  data[3] = n == 100U ? 1U : 2U;
  data[4] = 2U;  // direct custody; relay flag is NOT required
  data[5] = 1U;
  data[6] = n == 100U ? 64U : 48U;
  data[7] = 0U;
  assert(rec::productObjectShape(data.data(), data.size()));
  return data;
}
void expectedPayload(NorPage& nor, size_t offset,
                     const uint8_t* expected, size_t n) {
  std::array<uint8_t, page::kPageSize> bytes{};
  assert(offset <= bytes.size() && n <= bytes.size() - offset);
  assert(nor.read(0U, bytes.data(), bytes.size()));
  assert(memcmp(bytes.data() + offset, expected, n) == 0);
}

void testHappyAndCapacity() {
  NorPage nor;
  PageWriter writer(nor);
  assert(writer.inspect().state == PageState::kErased);
  const auto a = frame(84U);
  const auto b = frame(100U);
  assert(writer.appendExact(a.data(), a.size()) == WriteResult::kRefused);
  assert(writer.initializeEmpty(7U, false) == WriteResult::kRefused);
  assert(writer.inspect().state == PageState::kErased);
  assert(writer.initializeEmpty(7U, true) == WriteResult::kInitialized);
  const PageReport clean = writer.inspect();
  assert(clean.state == PageState::kActive && clean.generation == 7U);
  assert(clean.held == 0U && clean.first_erased_slot == 0U);
  assert(writer.initializeEmpty(9U, true) == WriteResult::kRefused);

  assert(writer.appendExact(a.data(), a.size()) == WriteResult::kStored);
  const size_t after_first = nor.programmedBytes();
  assert(writer.appendExact(a.data(), a.size()) == WriteResult::kDuplicate);
  assert(nor.programmedBytes() == after_first);
  assert(writer.appendExact(b.data(), b.size()) == WriteResult::kStored);

  PageWriter reboot(nor);  // persistent NOR bytes; no RAM state survives
  PageReport r = reboot.inspect();
  assert(r.state == PageState::kActive && r.held == 2U);
  assert(r.first_erased_slot == 2U);
  assert(reboot.appendExact(a.data(), a.size()) == WriteResult::kDuplicate);
  for (unsigned slot = 2U; slot < page::kRecordsPerPage; ++slot) {
    const auto next = frame(100U, static_cast<uint8_t>(slot + 31U));
    assert(reboot.appendExact(next.data(), next.size()) ==
           WriteResult::kStored);
  }
  r = reboot.inspect();
  assert(r.held == page::kRecordsPerPage);
  assert(r.first_erased_slot == UINT16_MAX);
  const auto extra = frame(100U, 234U);
  assert(reboot.appendExact(extra.data(), extra.size()) == WriteResult::kFull);
  assert(reboot.inspect().held == page::kRecordsPerPage);
}

void testHeaderCutAtEveryByte() {
  // 64 staging bytes, 4 commit, 4 activation. Include fully erased and
  // fully completed boundaries, with a NEW PageWriter after power loss.
  for (size_t budget = 0U; budget <= 72U; ++budget) {
    NorPage nor;
    nor.setProgramByteBudget(budget);
    PageWriter initial(nor);
    const WriteResult write = initial.initializeEmpty(13U, true);
    assert(write == (budget == 72U ? WriteResult::kInitialized
                                    : WriteResult::kPowerCut));
    assert(nor.programmedBytes() == budget);
    nor.clearFaults();
    PageWriter reboot(nor);
    const PageReport recovered = reboot.inspect();
    if (budget == 72U) {
      assert(recovered.state == PageState::kActive);
      assert(recovered.generation == 13U);
    } else {
      assert(recovered.state != PageState::kActive);
      assert(recovered.held == 0U);
      if (budget != 0U)
        assert(reboot.initializeEmpty(99U, true) == WriteResult::kRefused);
    }
  }
}

void testRecordCutAtEveryByte() {
  const auto a = frame(100U, 44U);
  // Record staging writes 120 bytes, then the separate 4-byte commit.
  for (size_t budget = 0U; budget <= 124U; ++budget) {
    NorPage nor;
    PageWriter first(nor);
    assert(first.initializeEmpty(3U, true) == WriteResult::kInitialized);
    nor.setProgramByteBudget(budget);
    const WriteResult write = first.appendExact(a.data(), a.size());
    assert(write == (budget == 124U ? WriteResult::kStored
                                     : WriteResult::kPowerCut));
    assert(nor.programmedBytes() == budget);
    nor.clearFaults();
    PageWriter reboot(nor);
    const PageReport report = reboot.inspect();
    if (budget == 124U) {
      assert(report.state == PageState::kActive && report.held == 1U);
      assert(reboot.appendExact(a.data(), a.size()) ==
             WriteResult::kDuplicate);
    } else {
      assert(report.held == 0U);
      if (budget == 0U) {
        assert(report.state == PageState::kActive);
        assert(report.first_erased_slot == 0U);
      } else {
        assert(report.state == PageState::kRefused);
        // Never skip or reuse a partially written record.
        assert(reboot.appendExact(a.data(), a.size()) ==
               WriteResult::kRefused);
      }
    }
  }
}

void testReadbackFailureAndReboot() {
  const auto a = frame(84U);
  // READ #1: erased census; #2: staged verify; #3: prepared verify;
  // #4: active verify. Fail closed even if media actually became ACTIVE.
  for (size_t reads : {size_t(0U), size_t(1U), size_t(2U),
                       size_t(3U)}) {
    NorPage nor;
    nor.setReadBudget(reads);
    PageWriter writer(nor);
    assert(writer.initializeEmpty(17U, true) == WriteResult::kReadFailure);
    nor.clearFaults();
    PageWriter reboot(nor);
    assert(reboot.inspect().state ==
           (reads == 3U ? PageState::kActive :
                          (reads == 0U ? PageState::kErased :
                                         PageState::kRefused)));
  }
  // READ #1: active/slots scan, #2: staged verify, #3: committed verify.
  for (size_t reads : {size_t(0U), size_t(1U), size_t(2U)}) {
    NorPage nor;
    PageWriter writer(nor);
    assert(writer.initializeEmpty(5U, true) == WriteResult::kInitialized);
    nor.setReadBudget(reads);
    assert(writer.appendExact(a.data(), a.size()) ==
           WriteResult::kReadFailure);
    nor.clearFaults();
    PageWriter reboot(nor);
    const PageReport r = reboot.inspect();
    assert(r.held == (reads == 2U ? 1U : 0U));
    if (reads == 1U) assert(r.state == PageState::kRefused);
  }
}

void testNorAndForeignPreservation() {
  NorPage nor;
  PageWriter writer(nor);
  const uint8_t zero[] = {0x00U};
  assert(nor.program(10U, zero, 1U));
  const size_t at_before = nor.programmedBytes();
  const uint8_t ff[] = {0xFFU};
  assert(!nor.program(10U, ff, 1U));  // NOR cannot set 0 back to 1
  assert(nor.programmedBytes() == at_before);
  expectedPayload(nor, 10U, zero, 1U);
  assert(writer.initializeEmpty(1U, true) == WriteResult::kRefused);

  NorPage legacy;
  const uint8_t old_magic[] = {'O', 'C', 'Q', '1'};
  assert(legacy.program(0U, old_magic, sizeof(old_magic)));
  PageWriter legacy_writer(legacy);
  assert(legacy_writer.inspect().state == PageState::kRefused);
  assert(legacy_writer.initializeEmpty(2U, true) == WriteResult::kRefused);
  expectedPayload(legacy, 0U, old_magic, sizeof(old_magic));

  NorPage guard;
  PageWriter guard_writer(guard);
  assert(guard_writer.initializeEmpty(2U, true) ==
         WriteResult::kInitialized);
  const uint8_t altered[] = {0xFEU};
  constexpr size_t kGuardStart =
      page::kHeaderSize + page::kRecordsPerPage * page::kRecordSize;
  assert(guard.program(kGuardStart, altered, sizeof(altered)));
  assert(guard_writer.inspect().state == PageState::kRefused);
  const auto object = frame(84U);
  assert(guard_writer.appendExact(object.data(), object.size()) ==
         WriteResult::kRefused);
}

}  // namespace

int main() {
  testHappyAndCapacity();
  testHeaderCutAtEveryByte();
  testRecordCutAtEveryByte();
  testReadbackFailureAndReboot();
  testNorAndForeignPreservation();
  puts("SF5G NOR page writer: byte-cut/readback/reboot/foreign/fill checks: PASS");
}
