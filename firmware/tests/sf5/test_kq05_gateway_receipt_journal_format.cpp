// PC-only byte-array NOR test for the new 8 KiB receipt storage proposal.
// No physical Flash, Gateway service, ACK, RF or trusted Edge enrollment.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_gateway_receipt_journal_format.h"

namespace journal = orun_tlp::kq05_receipt_journal;

namespace {

uint8_t flash[journal::kHistoryBytes];

bool program(uint32_t offset, const uint8_t* bytes, size_t size) {
  if (bytes == nullptr || offset > journal::kReceiptPageCount *
          journal::kPageSize ||
      size > journal::kReceiptPageCount * journal::kPageSize - offset)
    return false;
  for (size_t n = 0; n < size; ++n)
    if ((flash[offset + n] & bytes[n]) != bytes[n]) return false;
  for (size_t n = 0; n < size; ++n) flash[offset + n] &= bytes[n];
  return true;
}

void makeOffer(uint64_t row, uint8_t (&offer)[96]) {
  memset(offer, 0xA5, sizeof(offer));
  offer[0] = 0x91U;
  offer[1] = 1U;
  offer[2] = 0x91U;
  const uint8_t gw[8] = {'G', 'A', 'T', 'E', 'W', 'A', 'Y', '1'};
  memcpy(offer + 3U, gw, sizeof(gw));
  for (unsigned n = 0; n < 8U; ++n)
    offer[11U + n] = uint8_t(row >> ((7U - n) * 8U));
  offer[19] = 44U;
  offer[20] = 2U;
  offer[21] = 9U;
  // Remaining bytes are synthetic; authentic HMAC is checked by the
  // existing offer verifier, NOT by this structural flash codec.
}

void testHeaderAndPowercut() {
  uint8_t header[journal::kPageHeaderSize]{};
  journal::HeaderView view{};
  assert(journal::inspectHeader(flash, sizeof(header), view) ==
         journal::Evidence::kErased);
  assert(!journal::encodeHeader(0U, 1U, header));
  assert(!journal::encodeHeader(1U, 0U, header));
  assert(journal::encodeHeader(0x4741544557415931ULL, 1U, header));
  assert(program(0U, header, 60U));
  assert(journal::inspectHeader(flash, sizeof(header), view) ==
         journal::Evidence::kIncomplete);
  uint8_t commit[4]{};
  journal::headerCommitMarker(commit);
  assert(program(60U, commit, sizeof(commit)));
  assert(journal::inspectHeader(flash, sizeof(header), view) ==
         journal::Evidence::kCommitted);
  assert(view.gateway_id == 0x4741544557415931ULL &&
         view.generation == 1U);

  // Cross-reboot reads cannot turn an uncommitted second page into authority.
  const uint32_t second_page = journal::kPageSize;
  assert(journal::encodeHeader(0x4741544557415931ULL, 2U, header));
  assert(program(second_page, header, 60U));
  assert(journal::inspectHeader(flash + second_page,
                                sizeof(header), view) ==
         journal::Evidence::kIncomplete);
  assert(program(second_page + 60U, commit, sizeof(commit)));
  assert(journal::inspectHeader(flash + second_page,
                                sizeof(header), view) ==
         journal::Evidence::kCommitted);
  assert(view.generation == 2U);
  uint8_t changed[journal::kPageHeaderSize]{};
  memcpy(changed, flash, sizeof(changed));
  changed[9] ^= 0x01U;
  assert(journal::inspectHeader(changed, sizeof(changed), view) ==
         journal::Evidence::kCorrupt);
}

void testOneCopyCapacityAndTornWrites() {
  assert(journal::kCapacity == 56U);
  assert(journal::kRecordsPerPage == 28U);
  assert(journal::kReservedPageCount == 5U);
  for (size_t n = 0; n < journal::kCapacity; ++n) {
    const size_t page = n / journal::kRecordsPerPage;
    const size_t slot = n % journal::kRecordsPerPage;
    const size_t offset = page * journal::kPageSize +
        journal::kPageHeaderSize + slot * journal::kRecordSize;
    assert(offset + journal::kRecordSize <=
           (page + 1U) * journal::kPageSize);
    uint8_t offer[96]{};
    makeOffer(n + 1U, offer);
    uint8_t record[journal::kRecordSize]{};
    uint8_t recovered[journal::kOfferMax]{};
    size_t recovered_size = 0U;
    assert(journal::encodeRecord(offer, sizeof(offer), record));
    assert(program(static_cast<uint32_t>(offset), record, 140U));
    assert(journal::inspectRecord(flash + offset, sizeof(record),
                                  recovered, recovered_size) ==
           journal::Evidence::kIncomplete);
    uint8_t commit[4]{};
    journal::recordCommitMarker(commit);
    assert(program(static_cast<uint32_t>(offset + 140U), commit,
                   sizeof(commit)));
    assert(journal::inspectRecord(flash + offset, sizeof(record),
                                  recovered, recovered_size) ==
           journal::Evidence::kCommitted);
    assert(recovered_size == sizeof(offer) &&
           memcmp(recovered, offer, sizeof(offer)) == 0);

    uint8_t corrupt[journal::kRecordSize]{};
    memcpy(corrupt, flash + offset, sizeof(corrupt));
    corrupt[12U] ^= 0x01U;
    assert(journal::inspectRecord(corrupt, sizeof(corrupt),
                                  recovered, recovered_size) ==
           journal::Evidence::kCorrupt);
  }

  // No slot 57, no page erase, and 20 KiB expansion area is never touched.
  uint8_t offer[96]{};
  makeOffer(57U, offer);
  uint8_t record[journal::kRecordSize]{};
  assert(journal::encodeRecord(offer, sizeof(offer), record));
  assert(!program(journal::kReceiptPageCount * journal::kPageSize,
                  record, sizeof(record)));
  for (size_t i = journal::kReceiptPageCount * journal::kPageSize;
       i < sizeof(flash); ++i)
    assert(flash[i] == 0xFFU);
}

void testMalformedAndTornRecords() {
  uint8_t offer[96]{};
  makeOffer(77U, offer);
  uint8_t record[journal::kRecordSize]{};
  assert(journal::encodeRecord(offer, sizeof(offer), record));
  uint8_t recovered[journal::kOfferMax]{};
  size_t size = 0U;
  // Simulated power cut before commit word: never a STAGED acknowledgement.
  assert(journal::inspectRecord(record, sizeof(record), recovered, size) ==
         journal::Evidence::kIncomplete);
  uint8_t commit[4]{};
  journal::recordCommitMarker(commit);
  memcpy(record + 140U, commit, sizeof(commit));
  assert(journal::inspectRecord(record, sizeof(record), recovered, size) ==
         journal::Evidence::kCommitted);
  record[4] = 0xFFU;  // Invalid length, commit exists.
  assert(journal::inspectRecord(record, sizeof(record), recovered, size) ==
         journal::Evidence::kCorrupt);
  assert(!journal::encodeRecord(offer, 95U, record));
  offer[0] = 0x92U;
  assert(!journal::encodeRecord(offer, sizeof(offer), record));
}

}  // namespace

int main() {
  memset(flash, 0xFF, sizeof(flash));
  testHeaderAndPowercut();
  testOneCopyCapacityAndTornWrites();
  testMalformedAndTornRecords();
  puts("KQ05 Gateway 8KiB 2-page / 56-record no-A/B commit-last NOR layout: PASS (HOST ONLY)");
  puts("NO FLASH REPARTITION, NO ERASE, NO RF, NO DEVICE FLASH");
  return 0;
}
