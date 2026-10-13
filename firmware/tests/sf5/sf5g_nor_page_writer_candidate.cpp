#include "sf5g_nor_page_writer_candidate.h"

#include <string.h>

namespace orun_tlp {
namespace sf5g_nor_writer_candidate {
namespace {
bool allErased(const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}
constexpr size_t kRecordsStart = page::kHeaderSize;
constexpr size_t kRecordsEnd =
    kRecordsStart + page::kRecordsPerPage * page::kRecordSize;
constexpr size_t kIntentStart =
    page::kPageSize - page::kIntentsPerPage * page::kIntentSize;
static_assert(kRecordsEnd + rec::kSeparationGap == kIntentStart,
              "host writer region boundary");
}  // namespace

NorPage::NorPage() { bytes_.fill(0xFFU); }

void NorPage::setProgramByteBudget(size_t budget) {
  program_budget_ = budget;
  programmed_bytes_ = 0U;
  power_lost_ = false;
}
void NorPage::setReadBudget(size_t reads) { read_budget_ = reads; }
void NorPage::clearFaults() {
  program_budget_ = SIZE_MAX;
  programmed_bytes_ = 0U;
  read_budget_ = SIZE_MAX;
  power_lost_ = false;
}

bool NorPage::read(size_t offset, uint8_t* out, size_t size) const {
  if (out == nullptr || offset > bytes_.size() ||
      size > bytes_.size() - offset || read_budget_ == 0U) return false;
  if (read_budget_ != SIZE_MAX) --read_budget_;
  memcpy(out, bytes_.data() + offset, size);
  return true;
}

bool NorPage::program(size_t offset, const uint8_t* data, size_t size) {
  if (power_lost_ || data == nullptr || offset > bytes_.size() ||
      size > bytes_.size() - offset) return false;
  // Verify that this is legal 1->0 programming before *any* mutation.
  for (size_t i = 0; i < size; ++i)
    if ((bytes_[offset + i] & data[i]) != data[i]) return false;
  for (size_t i = 0; i < size; ++i) {
    if (programmed_bytes_ >= program_budget_) {
      power_lost_ = true;
      return false;
    }
    bytes_[offset + i] &= data[i];
    ++programmed_bytes_;
  }
  return true;
}

bool PageWriter::readExact(size_t offset, uint8_t* out, size_t size) const {
  return nor_.read(offset, out, size);
}

bool PageWriter::reservedAreasErased(const uint8_t* bytes) const {
  return allErased(bytes + kRecordsEnd, page::kPageSize - kRecordsEnd);
}

WriteResult PageWriter::initializeEmpty(uint64_t generation,
                                        bool authorized) {
  if (!authorized) return WriteResult::kRefused;
  // Full-page inspection prevents treating a torn header or stray bytes
  // outside the header as an erased, safely owned page.
  std::array<uint8_t, page::kPageSize> before{};
  if (!readExact(0U, before.data(), before.size()))
    return WriteResult::kReadFailure;
  if (!allErased(before.data(), before.size()))
    return WriteResult::kRefused;

  std::array<uint8_t, page::kHeaderSize> staged{};
  if (!page::encodeStagedHeader(generation, staged.data(), staged.size()))
    return WriteResult::kRefused;
  if (!nor_.program(0U, staged.data(), staged.size()))
    return nor_.powerLost() ? WriteResult::kPowerCut : WriteResult::kRefused;

  std::array<uint8_t, page::kHeaderSize> verify{};
  if (!readExact(0U, verify.data(), verify.size()))
    return WriteResult::kReadFailure;
  if (verify != staged) return WriteResult::kRefused;

  const uint8_t marker[4] = {0U, 0U, 0U, 0U};
  if (!nor_.program(page::kCommitOffset, marker, sizeof(marker)))
    return nor_.powerLost() ? WriteResult::kPowerCut : WriteResult::kRefused;

  page::HeaderInspection inspected;
  if (!readExact(0U, verify.data(), verify.size()))
    return WriteResult::kReadFailure;
  if (!page::inspectHeader(verify.data(), verify.size(), inspected) ||
      inspected.evidence != page::HeaderEvidence::kPrepared ||
      inspected.generation != generation)
    return WriteResult::kRefused;

  if (!nor_.program(page::kActiveOffset, marker, sizeof(marker)))
    return nor_.powerLost() ? WriteResult::kPowerCut : WriteResult::kRefused;
  if (!readExact(0U, verify.data(), verify.size()))
    return WriteResult::kReadFailure;
  if (!page::inspectHeader(verify.data(), verify.size(), inspected) ||
      inspected.evidence != page::HeaderEvidence::kActive ||
      inspected.generation != generation)
    return WriteResult::kRefused;
  return WriteResult::kInitialized;
}

PageReport PageWriter::inspect() const {
  PageReport result;
  std::array<uint8_t, page::kPageSize> bytes{};
  if (!readExact(0U, bytes.data(), bytes.size())) {
    result.state = PageState::kReadFailure;
    return result;
  }
  if (allErased(bytes.data(), bytes.size())) {
    result.state = PageState::kErased;
    return result;
  }

  page::HeaderInspection head;
  if (!page::inspectHeader(bytes.data(), page::kHeaderSize, head) ||
      head.evidence != page::HeaderEvidence::kActive ||
      !reservedAreasErased(bytes.data())) return result;

  bool saw_erased = false;
  for (size_t i = 0U; i < page::kRecordsPerPage; ++i) {
    rec::RecordInspection entry;
    if (!rec::inspectRecord(bytes.data() + kRecordsStart +
                                i * page::kRecordSize,
                            page::kRecordSize, entry)) return result;
    if (entry.evidence == rec::Evidence::kErased) {
      if (!saw_erased) {
        result.first_erased_slot = static_cast<uint16_t>(i);
        saw_erased = true;
      }
    } else if (saw_erased) {
      // Holes before committed slots violate FIFO append ordering.
      return PageReport();
    } else if (entry.evidence == rec::Evidence::kHeld) {
      ++result.held;
    } else if (entry.evidence == rec::Evidence::kHandedOff) {
      ++result.handed_off;
    } else {
      // Torn, staged or corrupt entries must never be reused or skipped.
      return PageReport();
    }
  }
  result.state = PageState::kActive;
  result.generation = head.generation;
  return result;
}

WriteResult PageWriter::appendExact(const uint8_t* object,
                                    size_t object_size) {
  std::array<uint8_t, rec::kRecordSize> staged{};
  if (!rec::encodeStagedRecord(object, object_size,
                               staged.data(), staged.size()))
    return WriteResult::kRefused;

  // Single snapshot controls duplicate checks and the only free slot.
  std::array<uint8_t, page::kPageSize> bytes{};
  if (!readExact(0U, bytes.data(), bytes.size()))
    return WriteResult::kReadFailure;
  page::HeaderInspection header;
  if (!page::inspectHeader(bytes.data(), page::kHeaderSize, header) ||
      header.evidence != page::HeaderEvidence::kActive ||
      !reservedAreasErased(bytes.data()))
    return WriteResult::kRefused;

  size_t next = page::kRecordsPerPage;
  bool saw_erased = false;
  bool duplicate = false;
  for (size_t i = 0; i < page::kRecordsPerPage; ++i) {
    rec::RecordInspection found;
    if (!rec::inspectRecord(bytes.data() + kRecordsStart +
                                i * page::kRecordSize,
                            page::kRecordSize, found))
      return WriteResult::kRefused;
    if (found.evidence == rec::Evidence::kErased) {
      if (!saw_erased) next = i;
      saw_erased = true;
    } else if (saw_erased) {
      return WriteResult::kRefused;
    } else if (found.evidence == rec::Evidence::kHeld ||
               found.evidence == rec::Evidence::kHandedOff) {
      duplicate |= rec::sameExactObject(found, object, object_size);
    } else {
      return WriteResult::kRefused;
    }
  }
  // Full exact-byte equality is only local deduplication, never an ACK.
  if (duplicate) return WriteResult::kDuplicate;
  if (next == page::kRecordsPerPage) return WriteResult::kFull;

  const size_t offset = kRecordsStart + next * page::kRecordSize;
  if (!nor_.program(offset, staged.data(), staged.size()))
    return nor_.powerLost() ? WriteResult::kPowerCut : WriteResult::kRefused;
  std::array<uint8_t, rec::kRecordSize> verify{};
  if (!readExact(offset, verify.data(), verify.size()))
    return WriteResult::kReadFailure;
  if (verify != staged) return WriteResult::kRefused;

  const uint8_t marker[4] = {0U, 0U, 0U, 0U};
  if (!nor_.program(offset + rec::kRecordCommitOffset,
                    marker, sizeof(marker)))
    return nor_.powerLost() ? WriteResult::kPowerCut : WriteResult::kRefused;

  if (!readExact(offset, verify.data(), verify.size()))
    return WriteResult::kReadFailure;
  rec::RecordInspection found;
  if (!rec::inspectRecord(verify.data(), verify.size(), found) ||
      found.evidence != rec::Evidence::kHeld ||
      !rec::sameExactObject(found, object, object_size))
    return WriteResult::kRefused;
  return WriteResult::kStored;
}

}  // namespace sf5g_nor_writer_candidate
}  // namespace orun_tlp
