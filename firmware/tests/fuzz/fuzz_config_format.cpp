#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config_format.h"

namespace cf = orun_tlp::config_format;

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
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size >= cf::kRecordSize) {
    uint64_t generation = 0;
    cf::Config config;
    if (cf::decode(data, generation, config)) {
      uint8_t encoded[cf::kRecordSize]{};
      cf::encode(config, generation, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size >= cf::kV2RecordSize) {
    cf::V2Record decoded;
    if (cf::decodeV2(data, decoded)) {
      uint8_t encoded[cf::kV2RecordSize]{};
      cf::encodeV2(decoded, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size >= cf::kV2PagePrefixSize) {
    cf::PageInspection inspection;
    if (!cf::inspectPagePrefix(data, cf::kV2PagePrefixSize, inspection))
      __builtin_trap();
  }

  if (size == 0) return 0;

  cf::V2Record candidate(
      fold64(data, size, 0) | 1ULL,
      cf::Config(fold32(data, size, 8), fold32(data, size, 12)),
      cf::StateToken(fold64(data, size, 16) | 1ULL,
                     fold32(data, size, 24) | 1U));

  uint8_t prefix[cf::kV2PagePrefixSize];
  memset(prefix, 0xFF, sizeof(prefix));
  cf::encodeV2(candidate, prefix);

  if (size > 1) {
    const size_t index = data[0] % sizeof(prefix);
    const uint8_t delta = data[1] == 0 ? 1U : data[1];
    prefix[index] ^= delta;
  }

  cf::V2Record decoded;
  (void)cf::decodeV2(prefix, decoded);
  cf::PageInspection inspection;
  if (!cf::inspectPagePrefix(prefix, sizeof(prefix), inspection))
    __builtin_trap();

  return 0;
}
