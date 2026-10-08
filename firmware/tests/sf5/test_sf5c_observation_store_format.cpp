#include <assert.h>
#include <string.h>

#include "observation_store_format.h"

namespace osf = orun_tlp::observation_store_format;

static void testGeometry() {
  static_assert(osf::kDataRecordsPerPage == 42U, "data geometry");
  static_assert(osf::kControlRecordsPerPage == 28U, "control geometry");
  static_assert(osf::kMaxExactCustodyObjects == 4U, "custody bound");
  assert(osf::kPageHeaderSize +
             osf::kDataRecordsPerPage * osf::kDataRecordSize ==
         osf::kPageSize);
  assert(osf::kPageHeaderSize +
             osf::kControlRecordsPerPage * osf::kControlRecordSize ==
         osf::kPageSize);
}

static void testPageHeader() {
  uint8_t bytes[osf::kPageHeaderSize];
  osf::encodePageHeader(osf::PageKind::kData, 7U, 0x1234U, 0x5678U,
                        bytes);

  osf::PageInspection page;
  assert(osf::inspectPageHeader(bytes, sizeof(bytes), page));
  assert(page.evidence == osf::PageEvidence::kPrepared);
  assert(page.kind == osf::PageKind::kData);
  assert(page.generation == 7U);
  assert(page.device_id == 0x1234U);
  assert(page.incarnation == 0x5678U);

  osf::put32(bytes + osf::kPageHeaderActiveOffset, osf::kActive);
  assert(osf::inspectPageHeader(bytes, sizeof(bytes), page));
  assert(page.evidence == osf::PageEvidence::kActive);

  bytes[60] = 0U;
  assert(osf::inspectPageHeader(bytes, sizeof(bytes), page));
  assert(page.evidence == osf::PageEvidence::kCorrupt);
}

static void testRecordCommitReleaseAndCanonicalPadding() {
  uint8_t payload[osf::kDataPayloadSize];
  for (unsigned i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(i);

  uint8_t bytes[osf::kDataRecordSize];
  assert(osf::encodeRecord(osf::RecordKind::kPeriodic, 1U, 0xAA55U, 9U,
                           payload, sizeof(payload), bytes));

  osf::RecordInspection record;
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kRetained);
  assert(record.kind == osf::RecordKind::kPeriodic);
  assert(record.incarnation == 0xAA55U);
  assert(record.sequence == 9U);
  assert(record.payload_size == sizeof(payload));
  assert(record.next_release_slot == 0U);
  assert(memcmp(record.payload, payload, sizeof(payload)) == 0);

  bytes[osf::kDataRecordRelease0Offset] = 0x7FU;
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kRetained);
  assert(record.release_uncertain);
  assert(record.next_release_slot == 1U);

  osf::put32(bytes + osf::kDataRecordRelease1Offset, osf::kReleased);
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kReleased);

  uint8_t short_payload[3] = {1U, 2U, 3U};
  assert(osf::encodeRecord(osf::RecordKind::kEvent, 1U, 1U, 1U,
                           short_payload, sizeof(short_payload), bytes));
  bytes[19] = 1U;  // Non-zero byte outside payload.
  osf::put32(bytes + osf::kDataRecordCrcOffset,
             osf::crc32(bytes, osf::kDataRecordCrcOffset));
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kCorrupt);
}

static void testRecordTornAndCorrupt() {
  uint8_t payload[4] = {9U, 8U, 7U, 6U};
  uint8_t bytes[osf::kDataRecordSize];
  assert(osf::encodeRecord(osf::RecordKind::kResult, 1U, 3U, 4U,
                           payload, sizeof(payload), bytes));

  osf::RecordInspection record;
  osf::put32(bytes + osf::kDataRecordCommitOffset, 0xFFFFFFFFU);
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kStaged);

  assert(osf::encodeRecord(osf::RecordKind::kResult, 1U, 3U, 4U,
                           payload, sizeof(payload), bytes));
  bytes[16] ^= 1U;
  assert(osf::inspectRecord(bytes, sizeof(bytes), record));
  assert(record.evidence == osf::RecordEvidence::kCorrupt);

  assert(!osf::encodeRecord(osf::RecordKind::kResult, 0U, 3U, 4U,
                            payload, sizeof(payload), bytes));
  assert(!osf::encodeRecord(osf::RecordKind::kResult, 1U, 0U, 4U,
                            payload, sizeof(payload), bytes));
  assert(!osf::encodeRecord(osf::RecordKind::kResult, 1U, 3U, 0U,
                            payload, sizeof(payload), bytes));
}

static void testControlCommitClearAndCorrupt() {
  uint8_t payload[osf::kControlPayloadSize];
  memset(payload, 0xA5, sizeof(payload));
  uint8_t bytes[osf::kControlRecordSize];

  assert(osf::encodeControl(osf::ControlKind::kExactObject, 1U, 3U,
                            payload, sizeof(payload), bytes));

  osf::ControlInspection control;
  assert(osf::inspectControl(bytes, sizeof(bytes), control));
  assert(control.evidence == osf::ControlEvidence::kActive);
  assert(control.kind == osf::ControlKind::kExactObject);
  assert(control.serial == 3U);
  assert(control.next_clear_slot == 0U);
  assert(memcmp(control.payload, payload, sizeof(payload)) == 0);

  bytes[osf::kControlRecordClear0Offset] = 0x7FU;
  assert(osf::inspectControl(bytes, sizeof(bytes), control));
  assert(control.evidence == osf::ControlEvidence::kActive);
  assert(control.clear_uncertain);
  assert(control.next_clear_slot == 1U);

  osf::put32(bytes + osf::kControlRecordClear1Offset, osf::kCleared);
  assert(osf::inspectControl(bytes, sizeof(bytes), control));
  assert(control.evidence == osf::ControlEvidence::kCleared);

  assert(osf::encodeControl(osf::ControlKind::kOpenOccurrence, 1U, 4U,
                            payload, 8U, bytes));
  bytes[40] = 1U;  // Non-zero canonical padding.
  osf::put32(bytes + osf::kControlRecordCrcOffset,
             osf::crc32(bytes, osf::kControlRecordCrcOffset));
  assert(osf::inspectControl(bytes, sizeof(bytes), control));
  assert(control.evidence == osf::ControlEvidence::kCorrupt);
}

int main() {
  testGeometry();
  testPageHeader();
  testRecordCommitReleaseAndCanonicalPadding();
  testRecordTornAndCorrupt();
  testControlCommitClearAndCorrupt();
  return 0;
}
