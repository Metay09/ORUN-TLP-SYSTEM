#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "geofence_format.h"

namespace gf = orun_tlp::geofence_format;

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
  if (size >= gf::kRecordSize) {
    gf::Record decoded;
    if (gf::decode(data, gf::kRecordSize, decoded)) {
      uint8_t encoded[gf::kRecordSize]{};
      if (!gf::encode(decoded, encoded, sizeof(encoded))) __builtin_trap();
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }

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

  uint8_t encoded[gf::kRecordSize]{};
  if (!gf::encode(candidate, encoded, sizeof(encoded))) __builtin_trap();

  if (size > 1) {
    const size_t index = data[0] % sizeof(encoded);
    const uint8_t delta = data[1] == 0 ? 1U : data[1];
    encoded[index] ^= delta;
  }

  gf::Record decoded;
  (void)gf::decode(encoded, sizeof(encoded), decoded);
  gf::PageInspection inspection;
  if (!gf::inspectPage(encoded, sizeof(encoded), inspection))
    __builtin_trap();

  return 0;
}
