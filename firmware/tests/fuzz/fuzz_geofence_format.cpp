#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "geofence_format.h"
#include "journal_format.h"

namespace gf = orun_tlp::geofence_format;
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

void reseal(uint8_t* bytes) {
  jf::put32(bytes + gf::kCrcOffset,
            jf::crc32(bytes, gf::kCrcOffset));
  jf::put32(bytes + gf::kCommitOffset, gf::kCommit);
}

void checkRoundTrip(const uint8_t* bytes) {
  gf::Record decoded;
  if (!gf::decode(bytes, gf::kRecordSize, decoded)) return;
  uint8_t encoded[gf::kRecordSize]{};
  if (!gf::encode(decoded, encoded, sizeof(encoded))) __builtin_trap();
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Raw fixed-record malformed-input path. The runner seeds a full 564-byte
  // record so this path is exercised immediately instead of relying on random
  // length growth.
  if (size >= gf::kRecordSize) {
    checkRoundTrip(data);
    gf::PageInspection inspection;
    if (!gf::inspectPage(data, gf::kRecordSize, inspection))
      __builtin_trap();
  }
  if (size == 0) return 0;

  gf::Record candidate;
  candidate.generation = fold64(data, size, 0) | 1ULL;
  candidate.token.incarnation = fold64(data, size, 8) | 1ULL;
  candidate.token.revision = fold32(data, size, 16) | 1U;

  if (data[0] & 1U) {
    const orun_tlp::GeoPointE7 vertices[3] = {
        orun_tlp::GeoPointE7(410000000, 290000000),
        orun_tlp::GeoPointE7(410010000, 290000000),
        orun_tlp::GeoPointE7(410000000, 290010000),
    };
    const orun_tlp::GeofencePolygonView polygon(vertices, 3);
    const orun_tlp::GeofenceAreaSetView area_set(&polygon, 1);
    if (!gf::canonicalizeConfiguredAreaSet(area_set, candidate.snapshot))
      __builtin_trap();
  } else {
    gf::makeClearSnapshot(candidate.snapshot);
  }

  uint8_t canonical[gf::kRecordSize]{};
  if (!gf::encode(candidate, canonical, sizeof(canonical))) __builtin_trap();
  checkRoundTrip(canonical);

  // Semantic mutation spans the complete body (including bytes >255), then
  // reseals CRC/commit so the decoder's semantic checks are reachable.
  uint8_t semantic[gf::kRecordSize];
  memcpy(semantic, canonical, sizeof(semantic));
  if (size > 1) {
    const size_t index = fold32(data, size, 20) % gf::kCrcOffset;
    semantic[index] ^= deltaByte(data, size, 24);
    reseal(semantic);
  }
  checkRoundTrip(semantic);
  gf::PageInspection semantic_inspection;
  if (!gf::inspectPage(semantic, sizeof(semantic), semantic_inspection))
    __builtin_trap();

  // Independent physical mutation spans CRC and commit too, without resealing,
  // so corruption/torn classifiers are exercised rather than hidden.
  uint8_t physical[gf::kRecordSize];
  memcpy(physical, canonical, sizeof(physical));
  if (size > 1) {
    const size_t index = fold32(data, size, 28) % sizeof(physical);
    physical[index] ^= deltaByte(data, size, 32);
  }
  gf::PageInspection physical_inspection;
  if (!gf::inspectPage(physical, sizeof(physical), physical_inspection))
    __builtin_trap();

  return 0;
}
