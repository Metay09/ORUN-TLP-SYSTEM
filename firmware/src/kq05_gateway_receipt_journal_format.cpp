#include "kq05_gateway_receipt_journal_format.h"

#include <string.h>

namespace orun_tlp {
namespace kq05_receipt_journal {
namespace {

constexpr uint8_t kHeaderMagic[4] = {'K', 'Q', '5', 'P'};
constexpr uint8_t kHeaderCommit[4] = {'K', 'Q', '5', 'H'};
constexpr uint8_t kRecordMagic[4] = {'K', 'Q', '5', 'R'};
constexpr uint8_t kRecordCommit[4] = {'K', 'Q', '5', 'C'};
constexpr size_t kHeaderCrcOffset = 52U;
constexpr size_t kHeaderCommitOffset = 60U;
constexpr size_t kRecordCrcOffset = 132U;
constexpr size_t kRecordCommitOffset = 140U;

bool allErased(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (data[i] != 0xFFU) return false;
  }
  return true;
}

uint64_t be64(const uint8_t* p) {
  uint64_t out = 0U;
  for (size_t i = 0; i < 8U; ++i)
    out = (out << 8U) | p[i];
  return out;
}

uint64_t le64(const uint8_t* p) {
  uint64_t out = 0U;
  for (size_t i = 0; i < 8U; ++i)
    out |= uint64_t(p[i]) << (i * 8U);
  return out;
}

void putLe64(uint8_t* p, uint64_t value) {
  for (size_t i = 0; i < 8U; ++i)
    p[i] = uint8_t(value >> (i * 8U));
}

void putLe32(uint8_t* p, uint32_t value) {
  for (size_t i = 0; i < 4U; ++i)
    p[i] = uint8_t(value >> (i * 8U));
}

uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8U) |
         (uint32_t(p[2]) << 16U) | (uint32_t(p[3]) << 24U);
}

bool offerShape(const uint8_t* p, size_t n) {
  if (p == nullptr || n < kOfferMin || n > kOfferMax ||
      p[0] != 0x91U || p[1] != 1U || p[2] != 0x91U ||
      be64(p + 3U) == 0U || be64(p + 11U) == 0U ||
      p[19] < 44U || p[19] > 72U ||
      n != 1U + 19U + p[19] + 32U ||
      p[20] != 2U || p[21] != 9U)
    return false;
  return true;  // Structural, never HMAC authority.
}

}  // namespace

uint32_t crc32(const uint8_t* bytes, size_t size) {
  if (bytes == nullptr) return 0U;
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < size; ++i) {
    crc ^= uint32_t(bytes[i]);
    for (unsigned bit = 0U; bit < 8U; ++bit)
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc ^ 0xFFFFFFFFU;
}

bool encodeHeader(uint64_t gateway_id, uint64_t generation,
                  uint8_t (&out)[kPageHeaderSize]) {
  if (gateway_id == 0U || generation == 0U) return false;
  memset(out, 0xFF, sizeof(out));
  memcpy(out, kHeaderMagic, sizeof(kHeaderMagic));
  out[4] = 1U;
  putLe64(out + 8U, gateway_id);
  putLe64(out + 16U, generation);
  putLe32(out + kHeaderCrcOffset, crc32(out, kHeaderCrcOffset));
  // Commit is deliberately left erased until durable body readback.
  return true;
}

Evidence inspectHeader(const uint8_t* bytes, size_t size, HeaderView& view) {
  if (bytes == nullptr || size != kPageHeaderSize) return Evidence::kCorrupt;
  if (allErased(bytes, size)) return Evidence::kErased;
  if (memcmp(bytes + kHeaderCommitOffset, kHeaderCommit, 4U) != 0)
    return Evidence::kIncomplete;
  if (memcmp(bytes, kHeaderMagic, sizeof(kHeaderMagic)) != 0 ||
      bytes[4] != 1U || !allErased(bytes + 5U, 3U) ||
      !allErased(bytes + 24U, 28U) ||
      !allErased(bytes + 56U, 4U) ||
      le32(bytes + kHeaderCrcOffset) != crc32(bytes, kHeaderCrcOffset) ||
      le64(bytes + 8U) == 0U || le64(bytes + 16U) == 0U)
    return Evidence::kCorrupt;
  view.gateway_id = le64(bytes + 8U);
  view.generation = le64(bytes + 16U);
  return Evidence::kCommitted;
}

void headerCommitMarker(uint8_t (&out)[4]) {
  memcpy(out, kHeaderCommit, sizeof(out));
}

bool encodeRecord(const uint8_t* logical_offer, size_t size,
                  uint8_t (&out)[kRecordSize]) {
  if (!offerShape(logical_offer, size)) return false;
  memset(out, 0xFF, sizeof(out));
  memcpy(out, kRecordMagic, sizeof(kRecordMagic));
  out[4] = uint8_t(size);
  out[5] = 0U;
  out[6] = 1U;
  memcpy(out + 8U, logical_offer, size);
  putLe32(out + kRecordCrcOffset, crc32(out, kRecordCrcOffset));
  // Commit remains 0xFFFFFFFF while only body is programmed.
  return true;
}

Evidence inspectRecord(const uint8_t* bytes, size_t size,
                       uint8_t (&logical_offer)[kOfferMax],
                       size_t& offer_size) {
  if (bytes == nullptr || size != kRecordSize) return Evidence::kCorrupt;
  if (allErased(bytes, size)) return Evidence::kErased;
  if (memcmp(bytes + kRecordCommitOffset, kRecordCommit, 4U) != 0)
    return Evidence::kIncomplete;
  const size_t n = bytes[4];
  if (memcmp(bytes, kRecordMagic, sizeof(kRecordMagic)) != 0 ||
      bytes[5] != 0U || bytes[6] != 1U || bytes[7] != 0xFFU ||
      !offerShape(bytes + 8U, n) ||
      !allErased(bytes + 8U + n, kOfferMax - n) ||
      !allErased(bytes + 136U, 4U) ||
      le32(bytes + kRecordCrcOffset) != crc32(bytes, kRecordCrcOffset))
    return Evidence::kCorrupt;
  memcpy(logical_offer, bytes + 8U, n);
  offer_size = n;
  return Evidence::kCommitted;
}

void recordCommitMarker(uint8_t (&out)[4]) {
  memcpy(out, kRecordCommit, sizeof(out));
}

}  // namespace kq05_receipt_journal
}  // namespace orun_tlp
