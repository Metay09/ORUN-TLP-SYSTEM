#include "sf5g_region_writer_candidate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <array>
#include <vector>

using namespace orun_tlp::sf5g_region_candidate;

namespace {
std::vector<uint8_t> product(size_t n, uint8_t seed) {
  std::vector<uint8_t> frame(n);
  for (size_t i = 0U; i < n; ++i)
    frame[i] = static_cast<uint8_t>(i * 11U + seed);
  frame[0] = 2U;
  frame[1] = 4U;
  frame[2] = 2U;
  frame[3] = n == 100U ? 1U : 2U;
  frame[4] = 2U;
  frame[5] = 1U;
  frame[6] = n == 100U ? 64U : 48U;
  frame[7] = 0U;
  assert(rec::productObjectShape(frame.data(), frame.size()));
  return frame;
}

void fillPageZero(RegionMedia& media) {
  RegionWriter writer(media);
  assert(writer.initializeEmpty(true) == nor::WriteResult::kInitialized);
  for (unsigned i = 0U; i < page::kRecordsPerPage; ++i) {
    const auto frame = product(100U, static_cast<uint8_t>(11U + i));
    assert(writer.appendExact(frame.data(), frame.size(), true) ==
           nor::WriteResult::kStored);
  }
  const RegionReport r = writer.inspect();
  assert(r.state == RegionState::kActive && r.active_pages == 1U);
  assert(r.held == 32U && r.newest_page == 0U);
  assert(r.newest_first_erased_slot == UINT16_MAX);
}

void testRegionProgressAndFull() {
  RegionMedia media(3U);
  RegionWriter writer(media);
  const auto first = product(84U, 202U);
  assert(writer.inspect().state == RegionState::kErased);
  assert(writer.initializeEmpty(false) == nor::WriteResult::kRefused);
  assert(writer.appendExact(first.data(), first.size(), true) ==
         nor::WriteResult::kRefused);
  assert(writer.initializeEmpty(true) == nor::WriteResult::kInitialized);
  assert(writer.initializeEmpty(true) == nor::WriteResult::kRefused);

  for (unsigned i = 0U; i < page::kRecordsPerPage; ++i) {
    const auto item = product(100U, static_cast<uint8_t>(11U + i));
    assert(writer.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kStored);
  }
  auto report = writer.inspect();
  assert(report.active_pages == 1U && report.held == 32U);
  const auto on_second = product(100U, 91U);
  assert(writer.appendExact(on_second.data(), on_second.size(), false) ==
         nor::WriteResult::kRefused);
  assert(writer.appendExact(on_second.data(), on_second.size(), true) ==
         nor::WriteResult::kStored);
  report = writer.inspect();
  assert(report.active_pages == 2U && report.newest_page == 1U);
  assert(report.newest_generation == 2U && report.held == 33U);
  assert(report.newest_first_erased_slot == 1U);

  RegionWriter reboot(media);
  const auto old = product(100U, 11U);
  assert(reboot.appendExact(old.data(), old.size(), true) ==
         nor::WriteResult::kDuplicate);
  assert(reboot.appendExact(on_second.data(), on_second.size(), true) ==
         nor::WriteResult::kDuplicate);

  // Fill the second page and the final physical page without erasing.
  for (unsigned i = 1U; i < page::kRecordsPerPage; ++i) {
    const auto item = product(100U, static_cast<uint8_t>(91U + i));
    assert(reboot.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kStored);
  }
  const auto page_three = product(84U, 192U);
  assert(reboot.appendExact(page_three.data(), page_three.size(), true) ==
         nor::WriteResult::kStored);
  report = reboot.inspect();
  assert(report.active_pages == 3U && report.newest_generation == 3U);
  assert(report.held == 65U && report.newest_first_erased_slot == 1U);
  for (unsigned i = 1U; i < page::kRecordsPerPage; ++i) {
    const auto item = product(100U, static_cast<uint8_t>(192U + i));
    assert(reboot.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kStored);
  }
  report = reboot.inspect();
  assert(report.held == 96U && report.newest_first_erased_slot == UINT16_MAX);
  const auto beyond = product(100U, 231U);
  assert(reboot.appendExact(beyond.data(), beyond.size(), true) ==
         nor::WriteResult::kFull);
  assert(reboot.appendExact(old.data(), old.size(), true) ==
         nor::WriteResult::kDuplicate);
  assert(reboot.inspect().held == 96U);
}

void testRotationAtEveryByteBoundary() {
  const auto extra = product(100U, 233U);
  // Rotation: 64 header bytes + 4 commit + 4 active,
  // followed by 120 record-stage bytes + 4 record-commit.
  for (size_t budget = 0U; budget <= 196U; ++budget) {
    RegionMedia media(2U);
    fillPageZero(media);
    nor::NorPage* next = media.pageAt(1U);
    assert(next != nullptr);
    next->setProgramByteBudget(budget);
    RegionWriter writer(media);
    const nor::WriteResult wrote =
        writer.appendExact(extra.data(), extra.size(), true);
    assert(wrote == (budget == 196U ? nor::WriteResult::kStored
                                    : nor::WriteResult::kPowerCut));
    assert(next->programmedBytes() == budget);
    next->clearFaults();
    RegionWriter reboot(media);
    const RegionReport state = reboot.inspect();

    if (budget == 0U) {
      assert(state.state == RegionState::kActive);
      assert(state.active_pages == 1U && state.held == 32U);
      assert(reboot.appendExact(extra.data(), extra.size(), true) ==
             nor::WriteResult::kStored);
    } else if (budget == 72U) {
      assert(state.state == RegionState::kActive);
      assert(state.active_pages == 2U && state.held == 32U);
      assert(reboot.appendExact(extra.data(), extra.size(), true) ==
             nor::WriteResult::kStored);
    } else if (budget == 196U) {
      assert(state.state == RegionState::kActive);
      assert(state.active_pages == 2U && state.held == 33U);
      assert(reboot.appendExact(extra.data(), extra.size(), true) ==
             nor::WriteResult::kDuplicate);
    } else {
      assert(state.state == RegionState::kRefused);
      assert(reboot.appendExact(extra.data(), extra.size(), true) ==
             nor::WriteResult::kRefused);
      assert(state.held == 0U); // refusal reports never promote partials
    }
  }
}

void testRegionRefusesForeignAndOrdering() {
  const auto item = product(84U, 88U);
  {
    RegionMedia media(3U);
    nor::NorPage* foreign = media.pageAt(1U);
    assert(foreign != nullptr);
    const uint8_t magic[] = {'O','C','Q','1'};
    assert(foreign->program(0U, magic, sizeof(magic)));
    RegionWriter writer(media);
    assert(writer.inspect().state == RegionState::kRefused);
    assert(writer.initializeEmpty(true) == nor::WriteResult::kRefused);
    assert(writer.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kRefused);
  }
  {
    RegionMedia media(3U);
    nor::NorPage* third = media.pageAt(2U);
    assert(third != nullptr);
    nor::PageWriter out_of_order(*third);
    assert(out_of_order.initializeEmpty(3U, true) ==
           nor::WriteResult::kInitialized);
    RegionWriter writer(media);
    assert(writer.inspect().state == RegionState::kRefused);
  }
  {
    RegionMedia media(3U);
    RegionWriter writer(media);
    assert(writer.initializeEmpty(true) == nor::WriteResult::kInitialized);
    // A second active page is impossible until page 0 is full.
    nor::PageWriter second(*media.pageAt(1U));
    assert(second.initializeEmpty(2U, true) ==
           nor::WriteResult::kInitialized);
    assert(writer.inspect().state == RegionState::kRefused);
  }
  {
    RegionMedia media(3U);
    fillPageZero(media);
    nor::PageWriter wrong_generation(*media.pageAt(1U));
    assert(wrong_generation.initializeEmpty(9U, true) ==
           nor::WriteResult::kInitialized);
    RegionWriter writer(media);
    assert(writer.inspect().state == RegionState::kRefused);
    assert(writer.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kRefused);
  }
  {
    RegionMedia media(2U);
    fillPageZero(media);
    media.pageAt(0U)->setReadBudget(0U);
    RegionWriter writer(media);
    assert(writer.inspect().state == RegionState::kReadFailure);
    assert(writer.appendExact(item.data(), item.size(), true) ==
           nor::WriteResult::kReadFailure);
    media.pageAt(0U)->clearFaults();
    assert(writer.inspect().state == RegionState::kActive);
  }
  {
    RegionMedia invalid(0U);
    RegionWriter writer(invalid);
    assert(writer.inspect().state == RegionState::kRefused);
    assert(writer.initializeEmpty(true) == nor::WriteResult::kRefused);
  }
}

} // namespace

int main() {
  testRegionProgressAndFull();
  testRotationAtEveryByteBoundary();
  testRegionRefusesForeignAndOrdering();
  puts("SF5G NOR region rotation: prefix-order/generation/dedup/cut/refusal: PASS");
}
