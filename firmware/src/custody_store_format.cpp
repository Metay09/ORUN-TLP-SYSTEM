#include "custody_store_format.h"

#include <string.h>

namespace orun_tlp::custody_store_format {

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
  put16(bytes + 6, static_cast<uint16_t>(kPageHeaderSize));
  put64(bytes + 8, generation);
  put16(bytes + 16, static_cast<uint16_t>(kRecordSize));
  put16(bytes + 18, static_cast<uint16_t>(kRecordsPerPage));
  put16(bytes + 20, static_cast<uint16_t>(kObjectSize));
  put16(bytes + 22, 0U);
  put32(bytes + kPageStaticCrcOffset, crc32(bytes, kPageStaticCrcOffset));
  put32(bytes + kPageHeaderCommitOffset, kCommit);
}

bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection) {
  inspection = {};
  if (bytes == nullptr || size != kPageHeaderSize) return false;
  if (erased(bytes, size)) {
    inspection.evidence = PageEvidence::kErased;
    return true;
  }
  if (get32(bytes) != kPageMagic) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }
  if (get16(bytes + 4) != kVersion) {
    inspection.evidence = PageEvidence::kUnsupported;
    return true;
  }
  if (get16(bytes + 6) != kPageHeaderSize ||
      get16(bytes + 16) != kRecordSize ||
      get16(bytes + 18) != kRecordsPerPage ||
      get16(bytes + 20) != kObjectSize || get16(bytes + 22) != 0U ||
      get32(bytes + kPageStaticCrcOffset) !=
          crc32(bytes, kPageStaticCrcOffset)) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }
  inspection.generation = get64(bytes + 8);
  if (inspection.generation == 0U) {
    inspection.evidence = PageEvidence::kCorrupt;
    return true;
  }

  const uint32_t commit = get32(bytes + kPageHeaderCommitOffset);
  const uint32_t active = get32(bytes + kPageHeaderActiveOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = PageEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = PageEvidence::kPartialCommit;
    return true;
  }
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
  memset(bytes, 0xFF, kReclaimIntentEnd - kReclaimIntentOffset);
  put16(bytes + 0, target_page);
  put16(bytes + 2, 0U);
  put64(bytes + 4, target_generation);
  put32(bytes + 12, crc32(bytes, 12U));
  put32(bytes + 16, kCommit);
}

bool inspectReclaimIntent(const uint8_t* page_header, size_t size,
                          ReclaimInspection& inspection) {
  inspection = {};
  if (page_header == nullptr || size != kPageHeaderSize) return false;
  const uint8_t* bytes = page_header + kReclaimIntentOffset;
  constexpr size_t kIntentSize = kReclaimIntentEnd - kReclaimIntentOffset;
  if (erased(bytes, kIntentSize)) {
    inspection.evidence = ReclaimEvidence::kNone;
    return true;
  }
  const uint32_t commit = get32(page_header + kReclaimIntentCommitOffset);
  if (commit == 0xFFFFFFFFU) {
    inspection.evidence = ReclaimEvidence::kStaged;
    return true;
  }
  if (commit != kCommit) {
    inspection.evidence = ReclaimEvidence::kPartialCommit;
    return true;
  }
  if (get16(bytes + 2) != 0U || get64(bytes + 4) == 0U ||
      get32(page_header + kReclaimIntentCrcOffset) != crc32(bytes, 12U)) {
    inspection.evidence = ReclaimEvidence::kCorrupt;
    return true;
  }
  inspection.evidence = ReclaimEvidence::kCommitted;
  inspection.target_page = get16(bytes + 0);
  inspection.target_generation = get64(bytes + 4);
  return true;
}

void encodeRecord(const uint8_t* object, size_t object_size, uint8_t* bytes) {
  memset(bytes, 0, kRecordSize);
  if (object != nullptr && object_size == kObjectSize)
    memcpy(bytes, object, kObjectSize);
  // The format version fixes object size at 73 bytes. Bytes 73..75 are zero
  // alignment padding so body+CRC is one word-aligned 80-byte program.
  put32(bytes + kRecordCrcOffset, crc32(bytes, kRecordCrcOffset));
  put32(bytes + kRecordCommitOffset, kCommit);
  memset(bytes + kRecordRetireOffset, 0xFF, 4);
}

bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection) {
  inspection = {};
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
  const uint32_t retired = get32(bytes + kRecordRetireOffset);
  if (retired == kHandedOff) {
    inspection.evidence = RecordEvidence::kHandedOff;
  } else {
    inspection.evidence = RecordEvidence::kHeld;
    inspection.retire_uncertain = retired != 0xFFFFFFFFU;
  }
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

}  // namespace orun_tlp::custody_store_format
