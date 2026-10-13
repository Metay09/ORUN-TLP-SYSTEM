#include "sf5g_region_writer_candidate.h"

namespace orun_tlp {
namespace sf5g_region_candidate {

RegionReport RegionWriter::inspectAndFind(const uint8_t* object, size_t size,
                                          bool* duplicate) const {
  RegionReport report;
  if (duplicate != nullptr) *duplicate = false;
  if (!media_.valid()) return report;

  bool saw_erased = false;
  bool previous_full = true;
  for (size_t index = 0U; index < media_.pageCount(); ++index) {
    nor::NorPage* physical = media_.pageAt(index);
    if (physical == nullptr) return RegionReport();
    nor::PageWriter writer(*physical);
    const nor::PageReport inspected = writer.inspect();
    if (inspected.state == nor::PageState::kReadFailure) {
      report.state = RegionState::kReadFailure;
      return report;
    }
    if (inspected.state == nor::PageState::kErased) {
      saw_erased = true;
      continue;
    }
    if (inspected.state != nor::PageState::kActive || saw_erased ||
        !previous_full ||
        inspected.generation != static_cast<uint64_t>(index) + 1U ||
        report.active_pages != index) {
      return RegionReport();  // never continue around a torn/foreign page
    }
    previous_full = inspected.first_erased_slot == UINT16_MAX;
    ++report.active_pages;
    report.newest_page = static_cast<uint16_t>(index);
    report.newest_generation = inspected.generation;
    report.newest_first_erased_slot = inspected.first_erased_slot;
    report.held = static_cast<uint16_t>(report.held + inspected.held);
    report.handed_off =
        static_cast<uint16_t>(report.handed_off + inspected.handed_off);

    if (object != nullptr) {
      // PageWriter::inspect already established header/record integrity;
      // independently re-read sealed bytes for cross-page full-byte dedup.
      // This is a synchronous, single-owner model (no concurrent mutation).
      std::array<uint8_t, page::kPageSize> bytes{};
      if (!physical->read(0U, bytes.data(), bytes.size())) {
        report.state = RegionState::kReadFailure;
        return report;
      }
      for (size_t slot = 0U; slot < page::kRecordsPerPage; ++slot) {
        rec::RecordInspection found;
        if (!rec::inspectRecord(
                bytes.data() + page::kHeaderSize + slot * page::kRecordSize,
                page::kRecordSize, found)) return RegionReport();
        if (duplicate != nullptr &&
            rec::sameExactObject(found, object, size)) {
          *duplicate = true;
        }
      }
    }
  }
  report.state =
      report.active_pages == 0U ? RegionState::kErased : RegionState::kActive;
  return report;
}

RegionReport RegionWriter::inspect() const {
  return inspectAndFind(nullptr, 0U, nullptr);
}

nor::WriteResult RegionWriter::initializeEmpty(bool owner_authorized) {
  if (!owner_authorized) return nor::WriteResult::kRefused;
  const RegionReport report = inspect();
  if (report.state == RegionState::kReadFailure)
    return nor::WriteResult::kReadFailure;
  if (report.state != RegionState::kErased)
    return nor::WriteResult::kRefused;
  nor::NorPage* first = media_.pageAt(0U);
  if (first == nullptr) return nor::WriteResult::kRefused;
  nor::PageWriter writer(*first);
  return writer.initializeEmpty(1U, true);
}

nor::WriteResult RegionWriter::appendExact(const uint8_t* object, size_t size,
                                            bool owner_authorized) {
  if (!owner_authorized || !rec::productObjectShape(object, size))
    return nor::WriteResult::kRefused;

  bool duplicate = false;
  const RegionReport report = inspectAndFind(object, size, &duplicate);
  if (report.state == RegionState::kReadFailure)
    return nor::WriteResult::kReadFailure;
  if (report.state != RegionState::kActive)
    return nor::WriteResult::kRefused;
  if (duplicate) return nor::WriteResult::kDuplicate;

  if (report.newest_page == UINT16_MAX) return nor::WriteResult::kRefused;
  if (report.newest_first_erased_slot != UINT16_MAX) {
    nor::NorPage* current = media_.pageAt(report.newest_page);
    if (current == nullptr) return nor::WriteResult::kRefused;
    nor::PageWriter writer(*current);
    return writer.appendExact(object, size);
  }

  const size_t next_page = size_t(report.newest_page) + 1U;
  if (next_page >= media_.pageCount()) return nor::WriteResult::kFull;
  if (report.newest_generation == UINT64_MAX) return nor::WriteResult::kRefused;
  nor::NorPage* next = media_.pageAt(next_page);
  if (next == nullptr) return nor::WriteResult::kRefused;
  nor::PageWriter writer(*next);
  const nor::WriteResult init =
      writer.initializeEmpty(report.newest_generation + 1U, true);
  if (init != nor::WriteResult::kInitialized) return init;
  return writer.appendExact(object, size);
}

}  // namespace sf5g_region_candidate
}  // namespace orun_tlp
