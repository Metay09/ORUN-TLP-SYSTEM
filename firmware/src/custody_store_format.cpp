#include "custody_store_format.h"

#include <string.h>

namespace orun_tlp {
namespace custody_store_format {

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

void put16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

void put32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24);
  data[1] = static_cast<uint8_t>(value >> 16);
  data[2] = static_cast<uint8_t>(value >> 8);
  data[3] = static_cast<uint8_t>(value);
}

void put64(uint8_t* data, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    data[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
}

uint16_t get16(const uint8_t* data) {
  return static_cast<uint16_t>((uint16_t(data[0]) << 8) | data[1]);
}

uint32_t get32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

uint64_t get64(const uint8_t* data) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value = (value << 8) | data[i];
  return value;
}

bool erased(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (data[i] != 0xFFU) return false;
  return true;
}

void encodePageHeader(uint64_t generation, uint8_t* bytes) {
  memset(bytes, 0xFF, kPageHeaderSize);
  put32(bytes + 0, kPageMagic);
  put16(bytes + 4, kVersion);
  put16(bytes + 6, static_cast<uint16_t>(~kVersion));
  put64(bytes + 8, generation);
  put16(bytes + 16, static_cast<uint16_t>(kPageHeaderSize));
  put16(bytes + 18, static_cast<uint16_t>(kRecordSize));
  put16(bytes + 20, static_cast<uint16_t>(kRecordsPerPage));
  put16(bytes + 22, static_cast<uint16_t>(kObjectSize));
  put16(bytes + 24, static_cast<uint16_t>(kIntentSlotSize));
  put16(bytes + 26, static_cast<uint16_t>(kIntentSlotsPerPage));
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

  if (get32(bytes) != kPageMagic) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  const uint16_t version = get16(bytes + 4);
  const uint16_t version_inverse = get16(bytes + 6);
  if (version_inverse != static_cast<uint16_t>(~version)) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }
  if (version != kVersion) {
    inspection.evidence = PageEvidence::kUnsupported;
    return true;
  }

  if (get16(bytes + 16) != kPageHeaderSize ||
      get16(bytes + 18) != kRecordSize ||
      get16(bytes + 20) != kRecordsPerPage ||
      get16(bytes + 22) != kObjectSize ||
      get16(bytes + 24) != kIntentSlotSize ||
      get16(bytes + 26) != kIntentSlotsPerPage ||
      get32(bytes + kPageStaticCrcOffset) !=
          crc32(bytes, kPageStaticCrcOffset)) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  for (uint32_t i = kPageHeaderActiveOffset + 4U;
       i < kPageHeaderSize; ++i) {
    if (bytes[i] != 0xFFU) {
      inspection.evidence = PageEvidence::kCorrupt;
      return true;
    }
  }

  inspection.generation = get64(bytes + 8);
  if (inspection.generation == 0U) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  const uint32_t active = get32(bytes + kPageHeaderActiveOffset);
  if (active == 0xFFFFFFFFU) {
    inspection.evidence = PageEvidence::kPrepared;
    return true;
  }
  if (active != kActive) {
    inspection.evidence = PageEvidence::kPartialActivation;
    return true;
  }
  inspection.evidence = PageEvidence::kActive;
  return true;
}

void encodeReclaimIntent(uint16_t target_page, uint64_t target_generation,
                         uint8_t* bytes) {
  memset(bytes, 0xFF, kIntentSlotSize);
  put16(bytes + 0, target_page);
  put16(bytes + 2, 0U);
  put64(bytes + 4, target_generation);
  put32(bytes + kIntentCrcOffset, crc32(bytes, kIntentCrcOffset));
  put32(bytes + kIntentCommitOffset, kCommit);
}

bool inspectReclaimIntent(const uint8_t* bytes, size_t size,
                          ReclaimInspection& inspection) {
  inspection = ReclaimInspection();
  if (bytes == nullptr || size != kIntentSlotSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = ReclaimEvidence::kNone;
    return true;
  }

  const uint32_t commit = get32(bytes + kIntentCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = ReclaimEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = ReclaimEvidence::kPartialCommit;
    return true;
  }

  if (get16(bytes + 2) != 0U || get64(bytes + 4) == 0U ||
      get32(bytes + kIntentCrcOffset) != crc32(bytes, kIntentCrcOffset)) {
    inspection.evidence = ReclaimEvidence::kCorrupt;
    return true;
  }

  inspection.target_page = get16(bytes + 0);
  inspection.target_generation = get64(bytes + 4);

  const uint32_t complete = get32(bytes + kIntentCompleteOffset);
  if (complete == 0xFFFFFFFFU) {
    inspection.evidence = ReclaimEvidence::kCommitted;
  } else if (complete == kIntentCompleted) {
    inspection.evidence = ReclaimEvidence::kCompleted;
  } else {
    inspection.evidence = ReclaimEvidence::kPartialCompletion;
  }
  return true;
}

void encodeRecord(const uint8_t* object, size_t object_size, uint8_t* bytes) {
  memset(bytes, 0, kRecordSize);
  if (object != nullptr && object_size == kObjectSize)
    memcpy(bytes, object, kObjectSize);
  put32(bytes + kRecordCrcOffset, crc32(bytes, kRecordCrcOffset));
  put32(bytes + kRecordCommitOffset, kCommit);
  memset(bytes + kRecordHandoff0Offset, 0xFF, 4U);
  memset(bytes + kRecordHandoff1Offset, 0xFF, 4U);
}

bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection) {
  inspection = RecordInspection();
  if (bytes == nullptr || size != kRecordSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = RecordEvidence::kErased;
    return true;
  }

  const uint32_t commit = get32(bytes + kRecordCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = RecordEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = RecordEvidence::kPartialCommit;
    return true;
  }

  for (uint32_t i = static_cast<uint32_t>(kObjectSize);
       i < kRecordCrcOffset; ++i) {
    if (bytes[i] != 0U) {
      inspection.evidence = RecordEvidence::kCorrupt;
      return true;
    }
  }
  if (get32(bytes + kRecordCrcOffset) != crc32(bytes, kRecordCrcOffset)) {
    inspection.evidence = RecordEvidence::kCorrupt;
    return true;
  }

  memcpy(inspection.object, bytes, kObjectSize);
  const uint32_t handoff0 = get32(bytes + kRecordHandoff0Offset);
  const uint32_t handoff1 = get32(bytes + kRecordHandoff1Offset);

  if (handoff0 == kHandedOff || handoff1 == kHandedOff) {
    inspection.evidence = RecordEvidence::kHandedOff;
    return true;
  }

  inspection.evidence = RecordEvidence::kHeld;
  inspection.handoff_uncertain =
      handoff0 != 0xFFFFFFFFU || handoff1 != 0xFFFFFFFFU;
  if (handoff0 == 0xFFFFFFFFU)
    inspection.next_handoff_slot = 0U;
  else if (handoff1 == 0xFFFFFFFFU)
    inspection.next_handoff_slot = 1U;
  return true;
}

bool exactObject(const RecordInspection& inspection,
                 const uint8_t* object, size_t object_size) {
  if (object == nullptr || object_size != kObjectSize) return false;
  if (inspection.evidence != RecordEvidence::kHeld &&
      inspection.evidence != RecordEvidence::kHandedOff)
    return false;
  return memcmp(inspection.object, object, kObjectSize) == 0;
}

}  // namespace custody_store_format
}  // namespace orun_tlp
