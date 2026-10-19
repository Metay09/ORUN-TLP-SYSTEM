#include "custody_store_product_format.h"

#include <string.h>

namespace orun_tlp {
namespace custody_store_product_format {
namespace {
uint32_t crc32(const uint8_t* input, size_t size) {
  uint32_t result = UINT32_C(0xFFFFFFFF);
  for (size_t i = 0U; i < size; ++i) {
    result ^= input[i];
    for (unsigned j = 0U; j < 8U; ++j)
      result = (result >> 1U) ^
               (UINT32_C(0xEDB88320) & (0U - (result & 1U)));
  }
  return result ^ UINT32_C(0xFFFFFFFF);
}
void put16(uint8_t* p, uint16_t n) {
  p[0] = static_cast<uint8_t>(n >> 8U);
  p[1] = static_cast<uint8_t>(n);
}
void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0U; i < 4U; ++i)
    p[i] = static_cast<uint8_t>(n >> (24U - 8U*i));
}
void put64(uint8_t* p, uint64_t n) {
  for (unsigned i = 0U; i < 8U; ++i)
    p[i] = static_cast<uint8_t>(n >> (56U - 8U*i));
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8U) | p[1]);
}
uint32_t get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
uint64_t get64(const uint8_t* p) {
  uint64_t out = 0U;
  for (unsigned i=0U; i<8U; ++i) out = (out << 8U) | p[i];
  return out;
}
bool erased(const uint8_t* bytes, size_t len) {
  for (size_t i=0U; i<len; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}
bool zeroed(const uint8_t* bytes, size_t len) {
  for (size_t i=0U; i<len; ++i)
    if (bytes[i] != 0x00U) return false;
  return true;
}
bool supportedSize(size_t size) {
  return size == kEventSize || size == kPeriodicSize;
}
} // namespace

bool encodePageHeader(uint64_t generation, uint8_t (&out)[kPageHeaderSize]) {
  if (generation == 0U || generation == UINT64_MAX) return false;
  uint8_t tmp[kPageHeaderSize];
  memset(tmp, 0xFF, sizeof(tmp));
  put32(tmp, kPageMagic);
  put16(tmp + 4U, kVersion);
  put16(tmp + 6U, static_cast<uint16_t>(~kVersion));
  put64(tmp + 8U, generation);
  put16(tmp + 16U, static_cast<uint16_t>(kPageHeaderSize));
  put16(tmp + 18U, static_cast<uint16_t>(kRecordSize));
  put16(tmp + 20U, static_cast<uint16_t>(kRecordsPerPage));
  put16(tmp + 22U, static_cast<uint16_t>(kMaxObjectSize));
  put16(tmp + 24U, static_cast<uint16_t>(kIntentSlotSize));
  put16(tmp + 26U, static_cast<uint16_t>(kIntentSlotsPerPage));
  put32(tmp + kHeaderCrcOffset, crc32(tmp, kHeaderCrcOffset));
  put32(tmp + kHeaderCommitOffset, 0U);
  // ACTIVE flag at 36..39 intentionally erased until separate flash commit.
  memcpy(out, tmp, sizeof(tmp));
  return true;
}

bool inspectPageHeader(const uint8_t* bytes, size_t size,
                       PageInspection& inspection) {
  if (bytes == nullptr || size != kPageHeaderSize) return false;
  PageInspection parsed{};
  if (erased(bytes, size)) {
    parsed.evidence = PageEvidence::kErased;
  } else {
    const uint32_t commit = get32(bytes + kHeaderCommitOffset);
    if (commit == UINT32_MAX) parsed.evidence = PageEvidence::kStaged;
    else if (commit != 0U) parsed.evidence = PageEvidence::kPartialCommit;
    else if (get32(bytes) != kPageMagic) parsed.evidence = PageEvidence::kCorrupt;
    else if (get16(bytes+4U) != kVersion)
      parsed.evidence = PageEvidence::kUnsupported;
    else if (get16(bytes+6U) != static_cast<uint16_t>(~kVersion) ||
             get64(bytes+8U) == 0U || get64(bytes+8U) == UINT64_MAX ||
             get16(bytes+16U) != kPageHeaderSize ||
             get16(bytes+18U) != kRecordSize ||
             get16(bytes+20U) != kRecordsPerPage ||
             get16(bytes+22U) != kMaxObjectSize ||
             get16(bytes+24U) != kIntentSlotSize ||
             get16(bytes+26U) != kIntentSlotsPerPage ||
             get32(bytes+kHeaderCrcOffset) != crc32(bytes,kHeaderCrcOffset) ||
             !erased(bytes+40U,24U))
      parsed.evidence = PageEvidence::kCorrupt;
    else {
      parsed.generation = get64(bytes+8U);
      const uint32_t active = get32(bytes+kHeaderActiveOffset);
      if (active == UINT32_MAX) parsed.evidence = PageEvidence::kPrepared;
      else if (active == 0U) parsed.evidence = PageEvidence::kActive;
      else parsed.evidence = PageEvidence::kPartialActivation;
    }
  }
  inspection = parsed;
  return true;
}

bool encodeRecord(const uint8_t* object, size_t size,
                  uint8_t (&out)[kRecordSize]) {
  if (object == nullptr || !supportedSize(size)) return false;
  uint8_t tmp[kRecordSize]{};
  memcpy(tmp, object, size);
  tmp[kObjectLengthOffset] = static_cast<uint8_t>(size);
  put32(tmp + kRecordCrcOffset, crc32(tmp,kRecordCrcOffset));
  put32(tmp + kRecordCommitOffset, 0U);
  memset(tmp + kRecordHandoff0Offset, 0xFF, 8U);
  memcpy(out, tmp, sizeof(tmp));
  return true;
}

bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& inspection) {
  if (bytes == nullptr || size != kRecordSize) return false;
  RecordInspection parsed{};
  if (erased(bytes, size)) {
    parsed.evidence = RecordEvidence::kErased;
  } else {
    const uint32_t committed = get32(bytes+kRecordCommitOffset);
    if (committed == UINT32_MAX) parsed.evidence = RecordEvidence::kStaged;
    else if (committed != 0U)
      parsed.evidence = RecordEvidence::kPartialCommit;
    else {
      const size_t length = bytes[kObjectLengthOffset];
      if (!supportedSize(length) ||
          !zeroed(bytes+length,kMaxObjectSize-length) ||
          !zeroed(bytes+kObjectLengthOffset+1U,3U) ||
          get32(bytes+kRecordCrcOffset) != crc32(bytes,kRecordCrcOffset)) {
        parsed.evidence = RecordEvidence::kCorrupt;
      } else {
        parsed.object_size = static_cast<uint8_t>(length);
        memcpy(parsed.object,bytes,length);
        const uint32_t marker0=get32(bytes+kRecordHandoff0Offset);
        const uint32_t marker1=get32(bytes+kRecordHandoff1Offset);
        if (marker0 == 0U || marker1 == 0U) {
          parsed.evidence = RecordEvidence::kHandedOff;
        } else {
          parsed.evidence = RecordEvidence::kHeld;
          parsed.handoff_uncertain =
              marker0 != UINT32_MAX || marker1 != UINT32_MAX;
          if (marker0 == UINT32_MAX) parsed.next_handoff_slot = 0U;
          else if (marker1 == UINT32_MAX) parsed.next_handoff_slot = 1U;
        }
      }
    }
  }
  inspection = parsed;
  return true;
}

bool exactObject(const RecordInspection& record, const uint8_t* object,
                 size_t size) {
  if (object == nullptr || !supportedSize(size) || record.object_size != size ||
      (record.evidence != RecordEvidence::kHeld &&
       record.evidence != RecordEvidence::kHandedOff)) return false;
  return memcmp(record.object,object,size) == 0;
}

} // namespace custody_store_product_format
} // namespace orun_tlp
