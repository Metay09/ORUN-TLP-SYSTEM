#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config_format.h"
#include "journal_format.h"

namespace cf = orun_tlp::config_format;
namespace jf = orun_tlp::journal_format;

namespace {
uint64_t fold64(const uint8_t* data, size_t size, size_t start) {
  uint64_t value = 0;
  if (size == 0) return 0;
  for (size_t i = 0; i < 8; ++i)
    value = (value << 8) | data[(start + i) % size];
  return value;
}

uint32_t fold32(const uint8_t* data, size_t size, size_t start) {
  uint32_t value = 0;
  if (size == 0) return 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | data[(start + i) % size];
  return value;
}

uint8_t deltaByte(const uint8_t* data, size_t size, size_t start) {
  if (size == 0) return 1;
  const uint8_t value = data[start % size];
  return value == 0 ? 1U : value;
}

void resealV1(uint8_t* bytes) {
  constexpr size_t kCrcOffset = 28;
  constexpr size_t kCommitOffset = 32;
  static_assert(kCommitOffset + 4 == cf::kRecordSize, "v1 config layout");
  jf::put32(bytes + kCrcOffset, jf::crc32(bytes, kCrcOffset));
  jf::put32(bytes + kCommitOffset, cf::kCommit);
}

void resealV2(uint8_t* bytes) {
  jf::put32(bytes + cf::kV2CrcOffset,
            jf::crc32(bytes, cf::kV2CrcOffset));
  jf::put32(bytes + cf::kV2CommitOffset, cf::kCommit);
}

void checkV1RoundTrip(const uint8_t* bytes) {
  uint64_t generation = 0;
  cf::Config config;
  if (!cf::decode(bytes, generation, config)) return;
  uint8_t encoded[cf::kRecordSize]{};
  cf::encode(config, generation, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}

void checkV2RoundTrip(const uint8_t* bytes) {
  cf::V2Record decoded;
  if (!cf::decodeV2(bytes, decoded)) return;
  uint8_t encoded[cf::kV2RecordSize]{};
  cf::encodeV2(decoded, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Raw malformed-input paths remain coverage-guided.
  if (size >= cf::kRecordSize) checkV1RoundTrip(data);
  if (size >= cf::kV2RecordSize) checkV2RoundTrip(data);
  if (size >= cf::kV2PagePrefixSize) {
    cf::PageInspection inspection;
    if (!cf::inspectPagePrefix(data, cf::kV2PagePrefixSize, inspection))
      __builtin_trap();
  }
  if (size == 0) return 0;

  const uint64_t generation = fold64(data, size, 0) | 1ULL;
  const cf::Config config(fold32(data, size, 8), fold32(data, size, 12));

  // Exercise accepted legacy-v1 decode and then mutate a body byte while
  // preserving CRC/commit so semantic validation, not CRC rejection, decides.
  uint8_t v1[cf::kRecordSize]{};
  cf::encode(config, generation, v1);
  checkV1RoundTrip(v1);
  if (size > 1) {
    constexpr size_t kV1BodySize = 28;
    const size_t index = fold32(data, size, 20) % kV1BodySize;
    v1[index] ^= deltaByte(data, size, 24);
    resealV1(v1);
    checkV1RoundTrip(v1);
  }

  const cf::V2Record candidate(
      generation, config,
      cf::StateToken(fold64(data, size, 16) | 1ULL,
                     fold32(data, size, 24) | 1U));

  uint8_t prefix[cf::kV2PagePrefixSize];
  memset(prefix, 0xFF, sizeof(prefix));
  cf::encodeV2(candidate, prefix);
  checkV2RoundTrip(prefix);

  // Semantic mutation: mutate anywhere in the v2 body, then recompute CRC and
  // restore commit. Accepted records must remain canonically round-trippable.
  uint8_t semantic[cf::kV2PagePrefixSize];
  memcpy(semantic, prefix, sizeof(semantic));
  if (size > 1) {
    const size_t index = fold32(data, size, 28) % cf::kV2CrcOffset;
    semantic[index] ^= deltaByte(data, size, 32);
    resealV2(semantic);
  }
  checkV2RoundTrip(semantic);
  cf::PageInspection semantic_inspection;
  if (!cf::inspectPagePrefix(semantic, sizeof(semantic), semantic_inspection))
    __builtin_trap();

  // Physical/torn mutation: allow CRC, commit and retire bytes themselves to
  // change without resealing so the page classifier sees power-cut/corruption
  // shapes as well.
  uint8_t physical[cf::kV2PagePrefixSize];
  memcpy(physical, prefix, sizeof(physical));
  if (size > 1) {
    const size_t index = fold32(data, size, 36) % sizeof(physical);
    physical[index] ^= deltaByte(data, size, 40);
  }
  cf::PageInspection physical_inspection;
  if (!cf::inspectPagePrefix(physical, sizeof(physical), physical_inspection))
    __builtin_trap();

  return 0;
}
