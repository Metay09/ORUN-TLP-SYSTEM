#include "sf5g_page_header_candidate.h"

#include <string.h>

namespace orun_tlp {
namespace sf5g_page_candidate {
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
void put64(uint8_t* p, uint64_t value) {
  for (unsigned i = 0; i != 8U; ++i)
    p[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}
uint32_t get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t get64(const uint8_t* p) {
  uint64_t value = 0U;
  for (unsigned i = 0; i != 8U; ++i) value = (value << 8) | p[i];
  return value;
}
bool erased(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i] != 0xFFU) return false;
  return true;
}
}  // namespace

bool encodeStagedHeader(uint64_t generation, uint8_t* out, size_t size) {
  if (out == nullptr || size != kHeaderSize || generation == 0U ||
      generation == UINT64_MAX) return false;
  uint8_t header[kHeaderSize];
  memset(header, 0xFF, sizeof(header));
  put32(header + 0U, kMagic);
  put16(header + 4U, kVersion);
  put16(header + 6U, static_cast<uint16_t>(~kVersion));
  put64(header + 8U, generation);
  put16(header + 16U, static_cast<uint16_t>(kHeaderSize));
  put16(header + 18U, static_cast<uint16_t>(kRecordSize));
  put16(header + 20U, static_cast<uint16_t>(kRecordsPerPage));
  put16(header + 22U, static_cast<uint16_t>(kObjectMax));
  put16(header + 24U, static_cast<uint16_t>(kIntentSize));
  put16(header + 26U, static_cast<uint16_t>(kIntentsPerPage));
  put32(header + kStaticCrcOffset,
        record::crc32(header, kStaticCrcOffset));
  // Commit and activation remain erased; cannot use this output as an
  // initialized page, regardless of any currently persisted bytes.
  memcpy(out, header, sizeof(header));
  return true;
}

bool inspectHeader(const uint8_t* bytes, size_t size,
                   HeaderInspection& out) {
  out = HeaderInspection();
  if (bytes == nullptr || size != kHeaderSize) return false;
  if (erased(bytes, size)) {
    out.evidence = HeaderEvidence::kErased;
    return true;
  }
  // Refuse legacy SF4B OCQ1, SF5C OBS1, and all unknown magic values.
  // This prefilter never reinterprets an alien region as an empty page.
  if (get32(bytes) != kMagic) {
    out.evidence = HeaderEvidence::kForeign;
    return true;
  }

  const uint32_t commit = get32(bytes + kCommitOffset);
  if (commit == kErased) {
    out.evidence = HeaderEvidence::kStaged;
    return true;
  }
  if (commit != kCommitted) {
    out.evidence = HeaderEvidence::kPartialCommit;
    return true;
  }
  if (get16(bytes + 4U) != kVersion ||
      get16(bytes + 6U) != static_cast<uint16_t>(~kVersion) ||
      get64(bytes + 8U) == 0U || get64(bytes + 8U) == UINT64_MAX ||
      get16(bytes + 16U) != kHeaderSize ||
      get16(bytes + 18U) != kRecordSize ||
      get16(bytes + 20U) != kRecordsPerPage ||
      get16(bytes + 22U) != kObjectMax ||
      get16(bytes + 24U) != kIntentSize ||
      get16(bytes + 26U) != kIntentsPerPage ||
      get32(bytes + kStaticCrcOffset) !=
          record::crc32(bytes, kStaticCrcOffset) ||
      !erased(bytes + kReservedOffset, kHeaderSize - kReservedOffset)) {
    out.evidence = HeaderEvidence::kCorrupt;
    return true;
  }
  const uint32_t active = get32(bytes + kActiveOffset);
  if (active == kErased) {
    out.evidence = HeaderEvidence::kPrepared;
    out.generation = get64(bytes + 8U);
    return true;
  }
  if (active != kCommitted) {
    out.evidence = HeaderEvidence::kPartialActivation;
    return true;
  }
  out.evidence = HeaderEvidence::kActive;
  out.generation = get64(bytes + 8U);
  return true;
}

bool encodeStagedIntent(uint16_t page, uint64_t generation,
                        uint8_t* out, size_t size) {
  if (out == nullptr || size != kIntentSize || page == UINT16_MAX ||
      generation == 0U || generation == UINT64_MAX) return false;
  uint8_t intent[kIntentSize];
  memset(intent, 0xFF, sizeof(intent));
  put16(intent + 0U, page);
  put16(intent + 2U, 0U);
  put64(intent + 4U, generation);
  put32(intent + kIntentCrcOffset, record::crc32(intent, kIntentCrcOffset));
  memcpy(out, intent, sizeof(intent));
  return true;
}

bool inspectIntent(const uint8_t* bytes, size_t size, IntentInspection& out) {
  out = IntentInspection();
  if (bytes == nullptr || size != kIntentSize) return false;
  if (erased(bytes, size)) {
    out.evidence = IntentEvidence::kErased;
    return true;
  }
  const uint32_t commit = get32(bytes + kIntentCommitOffset);
  if (commit == kErased) {
    out.evidence = IntentEvidence::kStaged;
    return true;
  }
  if (commit != kCommitted) {
    out.evidence = IntentEvidence::kPartialCommit;
    return true;
  }
  if (get16(bytes + 0U) == UINT16_MAX ||
      get16(bytes + 2U) != 0U ||
      get64(bytes + 4U) == 0U || get64(bytes + 4U) == UINT64_MAX ||
      get32(bytes + kIntentCrcOffset) !=
          record::crc32(bytes, kIntentCrcOffset)) {
    out.evidence = IntentEvidence::kCorrupt;
    return true;
  }
  const uint32_t done = get32(bytes + kIntentDoneOffset);
  if (done != kErased && done != kCommitted) {
    out.evidence = IntentEvidence::kPartialCompletion;
    return true;
  }
  out.target_page = get16(bytes);
  out.target_generation = get64(bytes + 4U);
  out.evidence = done == kCommitted ? IntentEvidence::kCompleted
                                    : IntentEvidence::kCommitted;
  return true;
}

}  // namespace sf5g_page_candidate
}  // namespace orun_tlp
