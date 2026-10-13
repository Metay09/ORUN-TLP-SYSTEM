#include "sf5g_intent_census_candidate.h"

#include <assert.h>
#include <stdio.h>

#include <array>

using namespace orun_tlp::sf5g_intent_census_candidate;

namespace {
constexpr size_t kArea =
    format::kPageSize - format::kIntentSize * format::kIntentsPerPage;
using Intent = std::array<uint8_t, format::kIntentSize>;

Intent staged(uint16_t target, uint64_t generation) {
  Intent bytes{};
  assert(format::encodeStagedIntent(target, generation,
                                    bytes.data(), bytes.size()));
  return bytes;
}

void putSlot(nor::NorPage& media, size_t slot, const Intent& raw,
             bool commit, bool done) {
  assert(slot < format::kIntentsPerPage);
  const size_t offset = kArea + slot * format::kIntentSize;
  assert(media.program(offset, raw.data(), raw.size()));
  const uint8_t marker[4] = {0U, 0U, 0U, 0U};
  if (commit)
    assert(media.program(offset + format::kIntentCommitOffset,
                         marker, sizeof(marker)));
  if (done) {
    assert(commit);
    assert(media.program(offset + format::kIntentDoneOffset,
                         marker, sizeof(marker)));
  }
}

void testProgressAndExhaustion() {
  nor::NorPage media;
  JournalReport r = inspectJournal(media, 0U, 4U);
  assert(r.state == JournalState::kAvailable);
  assert(r.completed == 0U && r.erased == 3U);
  assert(r.next_free_slot == 0U);
  putSlot(media, 0U, staged(1U, 7U), true, true);
  r = inspectJournal(media, 0U, 4U);
  assert(r.state == JournalState::kAvailable);
  assert(r.completed == 1U && r.erased == 2U);
  assert(r.next_free_slot == 1U);

  putSlot(media, 1U, staged(2U, 8U), true, false);
  r = inspectJournal(media, 0U, 4U);
  assert(r.state == JournalState::kPending);
  assert(r.completed == 1U && r.erased == 1U);
  assert(r.next_free_slot == 2U);
  assert(r.pending_target_page == 2U && r.pending_target_generation == 8U);

  const uint8_t marker[4] = {0U, 0U, 0U, 0U};
  assert(media.program(kArea + format::kIntentSize +
                           format::kIntentDoneOffset,
                       marker, sizeof(marker)));
  putSlot(media, 2U, staged(3U, 9U), true, true);
  r = inspectJournal(media, 0U, 4U);
  assert(r.state == JournalState::kExhausted);
  assert(r.completed == format::kIntentsPerPage);
  assert(r.erased == 0U && r.next_free_slot == UINT8_MAX);
  assert(r.pending_target_page == UINT16_MAX);
}

void testEveryByteBoundary() {
  const Intent bytes = staged(2U, 19U);
  // Staging 24 attempted bytes; commit 4 bytes; completion 4 bytes.
  // A completed entry is STILL never evidence of authorized erase.
  const uint8_t marker[4] = {0U, 0U, 0U, 0U};
  for (size_t budget = 0U; budget <= 32U; ++budget) {
    nor::NorPage media;
    media.setProgramByteBudget(budget);
    bool wrote = media.program(kArea, bytes.data(), bytes.size());
    if (wrote)
      wrote = media.program(kArea + format::kIntentCommitOffset,
                            marker, sizeof(marker));
    if (wrote)
      wrote = media.program(kArea + format::kIntentDoneOffset,
                            marker, sizeof(marker));
    assert(wrote == (budget == 32U));
    assert(media.programmedBytes() == budget);
    media.clearFaults();  // model a reboot: NOR bytes remain as-is
    const JournalReport r = inspectJournal(media, 0U, 4U);
    if (budget == 0U || budget == 32U) {
      assert(r.state == JournalState::kAvailable);
      assert(r.completed == (budget == 32U ? 1U : 0U));
    } else if (budget == 28U) {
      assert(r.state == JournalState::kPending);
      assert(r.pending_target_page == 2U);
      assert(r.pending_target_generation == 19U);
    } else {
      assert(r.state == JournalState::kRefused);
    }
  }
}

void testRefusalAndReadFaults() {
  {
    nor::NorPage media;
    media.setReadBudget(0U);
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kReadFailure);
    media.clearFaults();
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kAvailable);
    assert(inspectJournal(media, 0U, 0U).state ==
           JournalState::kRefused);
    assert(inspectJournal(media, 0U, 33U).state ==
           JournalState::kRefused);
    assert(inspectJournal(media, 4U, 4U).state ==
           JournalState::kRefused);
  }
  {
    nor::NorPage media;
    putSlot(media, 0U, staged(0U, 7U), true, true);  // self-target
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);
  }
  {
    nor::NorPage media;
    putSlot(media, 0U, staged(31U, 7U), true, false);
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);  // valid codec, invalid physical page
    assert(inspectJournal(media, 0U, 32U).state ==
           JournalState::kPending);
  }
  {
    nor::NorPage media;
    putSlot(media, 1U, staged(2U, 7U), true, true);
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);  // hole before populated slot
  }
  {
    nor::NorPage media;
    putSlot(media, 0U, staged(2U, 7U), true, false);
    putSlot(media, 1U, staged(3U, 8U), true, true);
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);  // conflicting second request
  }
  {
    nor::NorPage media;
    putSlot(media, 0U, staged(2U, 7U), false, false);
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);  // uncommitted staging
  }
  {
    nor::NorPage media;
    putSlot(media, 0U, staged(2U, 7U), true, true);
    const uint8_t zero = 0U;
    assert(media.program(kArea + 7U, &zero, 1U));
    assert(inspectJournal(media, 0U, 4U).state ==
           JournalState::kRefused);  // CRC mismatch with committed markers
  }
}
}  // namespace

int main() {
  testProgressAndExhaustion();
  testEveryByteBoundary();
  testRefusalAndReadFaults();
  puts("SF5G reclaim intent census: exhausted/pending/torn/bounds/read-fault: PASS");
}
