#include "sf5g_custody_record_candidate.h"

#include <string.h>

namespace orun_tlp {
namespace sf5g_custody_candidate {
namespace {

void put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value >> 8);
  p[1] = static_cast<uint8_t>(value);
}
void put32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}
uint32_t get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
bool erased(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i] != 0xFFU) return false;
  return true;
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t n) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

bool productObjectShape(const uint8_t* p, size_t size) {
  if (p == nullptr || (size != 84U && size != 100U)) return false;
  if (p[0] != 2U || p[1] != 4U || p[2] != 2U ||
      p[5] != 1U || p[7] != 0U) return false;
  // PRODUCT_SECURE section 5: direct custody (0x02) and relayed custody
  // (0x03) are both valid. Admission here REQUIRES custody_requested;
  // relay_allowed is optional. Never accept non-custody best-effort frames.
  if ((p[4] & 0x02U) == 0U || (p[4] & ~0x03U) != 0U) return false;
  if (!((size == 100U && p[3] == 1U && p[6] == 64U) ||
        (size == 84U && p[3] == 2U && p[6] == 48U))) return false;
  // Invalid epoch and zero counter; bytes remain untrusted until AEAD.
  if (erased(p + 16U, 4U)) return false;
  bool zero_counter = true;
  for (size_t i = 20U; i < 28U; ++i) zero_counter &= p[i] == 0U;
  return !zero_counter;
}

bool encodeStagedRecord(const uint8_t* object, size_t size,
                        uint8_t* out, size_t output_size) {
  if (out == nullptr || output_size != kRecordSize ||
      !productObjectShape(object, size)) return false;
  uint8_t candidate[kRecordSize];
  memset(candidate, 0xFF, sizeof(candidate));
  put16(candidate + kRecordLengthOffset, static_cast<uint16_t>(size));
  put16(candidate + 2U, 0U);
  memcpy(candidate + kRecordObjectOffset, object, size);
  put32(candidate + kRecordCrcOffset, crc32(candidate, kRecordCrcOffset));
  // Deliberately keep commit/handoff markers erased here.
  memcpy(out, candidate, sizeof(candidate));
  return true;
}

bool inspectRecord(const uint8_t* bytes, size_t size,
                   RecordInspection& out) {
  out = RecordInspection();
  if (bytes == nullptr || size != kRecordSize) return false;
  if (erased(bytes, size)) {
    out.evidence = Evidence::kErased;
    return true;
  }
  const uint32_t commit = get32(bytes + kRecordCommitOffset);
  if (commit == kErasedMarker) {
    out.evidence = Evidence::kStaged;
    return true;
  }
  if (commit != kProgrammedMarker) {
    out.evidence = Evidence::kPartialCommit;
    return true;
  }
  const uint16_t n = get16(bytes);
  if (get16(bytes + 2U) != 0U || (n != 84U && n != 100U) ||
      !erased(bytes + kRecordObjectOffset + n, kObjectMax - n) ||
      get32(bytes + kRecordCrcOffset) != crc32(bytes, kRecordCrcOffset) ||
      !productObjectShape(bytes + kRecordObjectOffset, n)) {
    out.evidence = Evidence::kCorrupt;
    return true;
  }
  out.object_size = n;
  memcpy(out.object, bytes + kRecordObjectOffset, n);
  const uint32_t h0 = get32(bytes + kRecordHandoff0Offset);
  const uint32_t h1 = get32(bytes + kRecordHandoff1Offset);
  if (h0 == kProgrammedMarker || h1 == kProgrammedMarker) {
    out.evidence = Evidence::kHandedOff;
    return true;
  }
  out.evidence = Evidence::kHeld;
  out.handoff_uncertain = h0 != kErasedMarker || h1 != kErasedMarker;
  // An erased A with a torn B is impossible for a well-ordered writer.
  // Preserve the object, but offer NO slot instead of implicitly trusting
  // the impossible state. A future store must diagnose/repair it explicitly.
  if (h0 == kErasedMarker && h1 == kErasedMarker)
    out.next_handoff_slot = 0;
  else if (h0 != kErasedMarker && h1 == kErasedMarker)
    out.next_handoff_slot = 1;
  return true;
}

bool sameExactObject(const RecordInspection& found,
                     const uint8_t* object, size_t size) {
  // Never trust a caller-supplied inspection's length to bound our fixed
  // object buffer. The normal reader produces only 84 or 100 bytes, but this
  // helper must remain safe even with a corrupted or synthetic inspection.
  if (object == nullptr || (size != 84U && size != 100U) ||
      size > kObjectMax || found.object_size != size ||
      (found.evidence != Evidence::kHeld &&
       found.evidence != Evidence::kHandedOff)) return false;
  return memcmp(found.object, object, size) == 0;
}

}  // namespace sf5g_custody_candidate
}  // namespace orun_tlp
