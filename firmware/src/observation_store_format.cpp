#include "observation_store_format.h"

#include <string.h>

namespace orun_tlp {
namespace observation_store_format {
namespace {

bool validPageKind(uint8_t value) {
  return value == static_cast<uint8_t>(PageKind::kData) ||
         value == static_cast<uint8_t>(PageKind::kControl);
}

bool validRecordKind(uint8_t value) {
  return value == static_cast<uint8_t>(RecordKind::kPeriodic) ||
         value == static_cast<uint8_t>(RecordKind::kEvent) ||
         value == static_cast<uint8_t>(RecordKind::kResult);
}

bool validControlKind(uint8_t value) {
  return value == static_cast<uint8_t>(ControlKind::kExactObject) ||
         value == static_cast<uint8_t>(ControlKind::kOpenOccurrence) ||
         value == static_cast<uint8_t>(ControlKind::kResultGuard) ||
         value == static_cast<uint8_t>(ControlKind::kStoreState);
}

uint8_t expectedPayloadSizeV1(RecordKind kind) {
  switch (kind) {
    case RecordKind::kPeriodic:
      return kPeriodicPayloadSizeV1;
    case RecordKind::kEvent:
      return kEventPayloadSizeV1;
    case RecordKind::kResult:
      return kResultPayloadSizeV1;
  }
  return 0U;
}

uint16_t slotSizeFor(PageKind kind) {
  return kind == PageKind::kData ? static_cast<uint16_t>(kDataRecordSize)
                                 : static_cast<uint16_t>(kControlRecordSize);
}

uint16_t slotCountFor(PageKind kind) {
  return kind == PageKind::kData
             ? static_cast<uint16_t>(kDataRecordsPerPage)
             : static_cast<uint16_t>(kControlRecordsPerPage);
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8U; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

void put16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8U);
  data[1] = static_cast<uint8_t>(value);
}

void put32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24U);
  data[1] = static_cast<uint8_t>(value >> 16U);
  data[2] = static_cast<uint8_t>(value >> 8U);
  data[3] = static_cast<uint8_t>(value);
}

void put64(uint8_t* data, uint64_t value) {
  for (unsigned i = 0; i < 8U; ++i)
    data[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
}

uint16_t get16(const uint8_t* data) {
  return static_cast<uint16_t>((uint16_t(data[0]) << 8U) | data[1]);
}

uint32_t get32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24U) | (uint32_t(data[1]) << 16U) |
         (uint32_t(data[2]) << 8U) | uint32_t(data[3]);
}

uint64_t get64(const uint8_t* data) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8U; ++i) value = (value << 8U) | data[i];
  return value;
}

bool erased(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (data[i] != 0xFFU) return false;
  return true;
}

void encodePageHeader(PageKind kind, uint64_t generation, uint64_t device_id,
                      uint64_t incarnation, uint8_t* bytes) {
  memset(bytes, 0xFF, kPageHeaderSize);
  put32(bytes + 0U, kPageMagic);
  put16(bytes + 4U, kVersion);
  put16(bytes + 6U, static_cast<uint16_t>(~kVersion));
  put64(bytes + 8U, generation);
  put64(bytes + 16U, device_id);
  put64(bytes + 24U, incarnation);
  bytes[32] = static_cast<uint8_t>(kind);
  bytes[33] = bytes[34] = bytes[35] = 0U;
  put16(bytes + 36U, static_cast<uint16_t>(kPageHeaderSize));
  put16(bytes + 38U, slotSizeFor(kind));
  put16(bytes + 40U, slotCountFor(kind));
  put16(bytes + 42U, 0U);
  put32(bytes + kPageStaticCrcOffset, crc32(bytes, kPageStaticCrcOffset));
  put32(bytes + kPageHeaderCommitOffset, kCommit);
}

bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection) {
  inspection = PageInspection();
  if (bytes == nullptr || size != kPageHeaderSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = PageEvidence::kErased;
    return true;
  }

  const uint32_t commit = get32(bytes + kPageHeaderCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = PageEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = PageEvidence::kPartialCommit;
    return true;
  }

  if (get32(bytes + 0U) != kPageMagic) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }
  const uint16_t version = get16(bytes + 4U);
  if (get16(bytes + 6U) != static_cast<uint16_t>(~version)) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }
  if (version != kVersion) {
    inspection.evidence = PageEvidence::kUnsupported;
    return true;
  }
  if (!validPageKind(bytes[32])) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  const PageKind kind = static_cast<PageKind>(bytes[32]);
  if (bytes[33] != 0U || bytes[34] != 0U || bytes[35] != 0U ||
      get16(bytes + 36U) != kPageHeaderSize ||
      get16(bytes + 38U) != slotSizeFor(kind) ||
      get16(bytes + 40U) != slotCountFor(kind) ||
      get16(bytes + 42U) != 0U ||
      get32(bytes + kPageStaticCrcOffset) !=
          crc32(bytes, kPageStaticCrcOffset)) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  if (kind == PageKind::kControl) {
    const uint32_t intent = get32(bytes + kControlIntentOffset);
    const uint32_t retired = get32(bytes + kControlIntentRetiredOffset);
    if (intent == 0xFFFFFFFFU && retired == 0xFFFFFFFFU) {
      // No compaction has been initiated from this control generation.
    } else if (intent == kControlIntent) {
      // One-way retirement: ANY programmed bit proves that the retirement
      // write started, which occurs only after the new ACTIVE page commits.
      // This avoids rewriting a partly programmed NVMC word after power cut.
      // An all-FF marker is the only pending-retirement state.
      inspection.control_intent_present = true;
      inspection.control_intent_retired = retired != 0xFFFFFFFFU;
    } else {
      // A torn marker is not authorization to discard either page.
      inspection.evidence = PageEvidence::kCorrupt;
      return true;
    }
  } else {
    for (uint32_t i = kControlIntentOffset; i < kPageHeaderSize; ++i) {
      if (bytes[i] != 0xFFU) {
        inspection.evidence = PageEvidence::kCorrupt;
        return true;
      }
    }
  }

  inspection.kind = kind;
  inspection.generation = get64(bytes + 8U);
  inspection.device_id = get64(bytes + 16U);
  inspection.incarnation = get64(bytes + 24U);
  if (inspection.generation == 0U || inspection.device_id == 0U ||
      inspection.incarnation == 0U) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  const uint32_t active = get32(bytes + kPageHeaderActiveOffset);
  if (active == 0xFFFFFFFFU) {
    inspection.evidence = PageEvidence::kPrepared;
  } else if (active == kActive) {
    inspection.evidence = PageEvidence::kActive;
  } else {
    inspection.evidence = PageEvidence::kPartialActivation;
  }
  return true;
}

bool encodeRecord(RecordKind kind, uint8_t schema, uint64_t incarnation,
                  uint32_t sequence, const uint8_t* payload,
                  size_t payload_size, uint8_t* bytes) {
  if (bytes == nullptr || !validRecordKind(static_cast<uint8_t>(kind)) ||
      schema == 0U || incarnation == 0U || sequence == 0U ||
      payload_size > kDataPayloadSize ||
      (payload_size != 0U && payload == nullptr))
    return false;
  if (schema == kProductSchemaV1 &&
      payload_size != expectedPayloadSizeV1(kind))
    return false;

  memset(bytes, 0, kDataRecordSize);
  bytes[0] = static_cast<uint8_t>(kind);
  bytes[1] = schema;
  bytes[2] = static_cast<uint8_t>(payload_size);
  bytes[3] = 0U;
  put64(bytes + 4U, incarnation);
  put32(bytes + 12U, sequence);
  if (payload_size != 0U) memcpy(bytes + 16U, payload, payload_size);
  put32(bytes + kDataRecordCrcOffset, crc32(bytes, kDataRecordCrcOffset));
  put32(bytes + kDataRecordCommitOffset, kCommit);
  memset(bytes + kDataRecordRelease0Offset, 0xFF, 8U);
  return true;
}

bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection) {
  inspection = RecordInspection();
  if (bytes == nullptr || size != kDataRecordSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = RecordEvidence::kErased;
    return true;
  }

  const uint32_t commit = get32(bytes + kDataRecordCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = RecordEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = RecordEvidence::kPartialCommit;
    return true;
  }

  if (!validRecordKind(bytes[0]) || bytes[1] == 0U ||
      bytes[2] > kDataPayloadSize || bytes[3] != 0U ||
      get64(bytes + 4U) == 0U || get32(bytes + 12U) == 0U ||
      get32(bytes + kDataRecordCrcOffset) !=
          crc32(bytes, kDataRecordCrcOffset)) {
    inspection.evidence = RecordEvidence::kCorrupt;
    return true;
  }

  const RecordKind kind = static_cast<RecordKind>(bytes[0]);
  const uint8_t schema = bytes[1];
  const uint8_t payload_size = bytes[2];
  if (schema == kProductSchemaV1 &&
      payload_size != expectedPayloadSizeV1(kind)) {
    inspection.evidence = RecordEvidence::kCorrupt;
    return true;
  }
  for (uint32_t i = 16U + payload_size; i < kDataRecordCrcOffset; ++i) {
    if (bytes[i] != 0U) {
      inspection.evidence = RecordEvidence::kCorrupt;
      return true;
    }
  }

  inspection.kind = kind;
  inspection.schema = schema;
  inspection.payload_size = payload_size;
  inspection.incarnation = get64(bytes + 4U);
  inspection.sequence = get32(bytes + 12U);
  memcpy(inspection.payload, bytes + 16U, kDataPayloadSize);

  const uint32_t release0 = get32(bytes + kDataRecordRelease0Offset);
  const uint32_t release1 = get32(bytes + kDataRecordRelease1Offset);
  if (release0 == kReleased || release1 == kReleased) {
    inspection.evidence = RecordEvidence::kReleased;
    return true;
  }

  inspection.evidence = RecordEvidence::kRetained;
  inspection.release_uncertain =
      release0 != 0xFFFFFFFFU || release1 != 0xFFFFFFFFU;
  if (release0 == 0xFFFFFFFFU)
    inspection.next_release_slot = 0U;
  else if (release1 == 0xFFFFFFFFU)
    inspection.next_release_slot = 1U;
  return true;
}

bool encodeControl(ControlKind kind, uint8_t schema, uint64_t serial,
                   const uint8_t* payload, size_t payload_size,
                   uint8_t* bytes) {
  if (bytes == nullptr || !validControlKind(static_cast<uint8_t>(kind)) ||
      schema == 0U || serial == 0U || payload_size > kControlPayloadSize ||
      (payload_size != 0U && payload == nullptr))
    return false;

  memset(bytes, 0, kControlRecordSize);
  bytes[0] = static_cast<uint8_t>(kind);
  bytes[1] = schema;
  put16(bytes + 2U, static_cast<uint16_t>(payload_size));
  put64(bytes + 4U, serial);
  if (payload_size != 0U) memcpy(bytes + 12U, payload, payload_size);
  put32(bytes + kControlRecordCrcOffset,
        crc32(bytes, kControlRecordCrcOffset));
  put32(bytes + kControlRecordCommitOffset, kCommit);
  memset(bytes + kControlRecordClear0Offset, 0xFF, 8U);
  return true;
}

bool inspectControl(const uint8_t* bytes, size_t size,
                    ControlInspection& inspection) {
  inspection = ControlInspection();
  if (bytes == nullptr || size != kControlRecordSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = ControlEvidence::kErased;
    return true;
  }

  const uint32_t commit = get32(bytes + kControlRecordCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = ControlEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = ControlEvidence::kPartialCommit;
    return true;
  }

  const uint16_t payload_size = get16(bytes + 2U);
  if (!validControlKind(bytes[0]) || bytes[1] == 0U ||
      payload_size > kControlPayloadSize || get64(bytes + 4U) == 0U ||
      get32(bytes + kControlRecordCrcOffset) !=
          crc32(bytes, kControlRecordCrcOffset)) {
    inspection.evidence = ControlEvidence::kCorrupt;
    return true;
  }

  for (uint32_t i = 12U + payload_size; i < kControlRecordCrcOffset; ++i) {
    if (bytes[i] != 0U) {
      inspection.evidence = ControlEvidence::kCorrupt;
      return true;
    }
  }

  inspection.kind = static_cast<ControlKind>(bytes[0]);
  inspection.schema = bytes[1];
  inspection.payload_size = payload_size;
  inspection.serial = get64(bytes + 4U);
  memcpy(inspection.payload, bytes + 12U, kControlPayloadSize);

  const uint32_t clear0 = get32(bytes + kControlRecordClear0Offset);
  const uint32_t clear1 = get32(bytes + kControlRecordClear1Offset);
  if (clear0 == kCleared || clear1 == kCleared) {
    inspection.evidence = ControlEvidence::kCleared;
    return true;
  }

  inspection.evidence = ControlEvidence::kActive;
  inspection.clear_uncertain =
      clear0 != 0xFFFFFFFFU || clear1 != 0xFFFFFFFFU;
  if (clear0 == 0xFFFFFFFFU)
    inspection.next_clear_slot = 0U;
  else if (clear1 == 0xFFFFFFFFU)
    inspection.next_clear_slot = 1U;
  return true;
}

}  // namespace observation_store_format
}  // namespace orun_tlp
