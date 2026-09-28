#pragma once

#include <stddef.h>
#include <stdint.h>

#include "geofence_runtime.h"

namespace orun_tlp {
namespace geofence_format {

// M6D3A v1 is one fixed-size, commit-last snapshot record. It owns no flash
// backend and performs no mutation. The fixed layout deliberately leaves all
// 64 coordinate slots present so CRC/commit offsets never depend on geometry.
constexpr uint32_t kMagic = 0x4F524731UL;  // "ORG1"
constexpr uint8_t kVersion = 1;
constexpr uint32_t kCommit = 0;
constexpr uint32_t kErasedWord = UINT32_MAX;

constexpr uint8_t kMaximumAreas = 8;
constexpr uint16_t kMaximumTotalVertices = 64;
constexpr uint16_t kPayloadSize = 520;  // 8 area counts + 64 * 8-byte E7 points.

constexpr uint32_t kStateOffset = 28;
constexpr uint32_t kAreaCountOffset = 29;
constexpr uint32_t kTotalVertexCountOffset = 30;
constexpr uint32_t kPayloadSizeOffset = 32;
constexpr uint32_t kAreaVertexCountsOffset = 36;
constexpr uint32_t kVerticesOffset = 44;
constexpr uint32_t kCrcOffset = 556;
constexpr uint32_t kCommitOffset = 560;
constexpr uint32_t kRecordSize = 564;

static_assert(kMaximumAreas == geofence_runtime_config::kMaximumAreas,
              "durable area capacity must match bounded runtime capacity");
static_assert(kMaximumTotalVertices ==
                  geofence_runtime_config::kMaximumTotalVertices,
              "durable vertex capacity must match bounded runtime capacity");
static_assert(kVerticesOffset + kMaximumTotalVertices * 8U == kCrcOffset,
              "vertex payload packing");
static_assert(kRecordSize == kCommitOffset + 4U, "record packing");

enum class ResourceState : uint8_t {
  kClear = 0,
  kConfigured = 1,
};

struct StateToken {
  constexpr StateToken(uint64_t incarnation_value = 0,
                       uint32_t revision_value = 0)
      : incarnation(incarnation_value), revision(revision_value) {}
  uint64_t incarnation;
  uint32_t revision;
};

struct Snapshot {
  Snapshot();
  ResourceState state;
  uint8_t area_count;
  uint16_t total_vertex_count;
  uint8_t area_vertex_counts[kMaximumAreas];
  GeoPointE7 vertices[kMaximumTotalVertices];
};

struct Record {
  Record();
  uint64_t generation;
  StateToken token;
  Snapshot snapshot;
};

enum class PageEvidence : uint8_t {
  kErased,
  kStaged,
  kUncommittedOrTorn,
  kPartialCommit,
  kCommittedClear,
  kCommittedConfigured,
  kCommittedCorrupt,
  kUnsupportedNewer,
  kSupportedCorrupt,
};

struct PageInspection {
  PageInspection();
  PageEvidence evidence;
  bool has_decoded_record;
  Record record;
};

// Produce the one canonical CLEAR semantic snapshot.
void makeClearSnapshot(Snapshot& snapshot);

// Validate using the existing M6C geometry contract, strip an optional final
// closing duplicate, preserve polygon/vertex order, and flatten into the fixed
// 8-area / 64-total-vertex durable representation.
bool canonicalizeConfiguredAreaSet(const GeofenceAreaSetView& area_set,
                                   Snapshot& snapshot);

// encode() validates generation/token/snapshot before touching output.
bool encode(const Record& record, uint8_t* bytes, size_t size);

// Body decode validates magic/version/reserved bytes, payload length, CRC,
// token fields and complete geofence semantics but does not require commit=0.
bool decodeBody(const uint8_t* bytes, size_t size, Record& record);
bool decode(const uint8_t* bytes, size_t size, Record& record);

// Evidence classifier for one page prefix containing the complete fixed v1
// record. Unsupported/newer evidence is never guessed as CLEAR.
bool inspectPage(const uint8_t* bytes, size_t size, PageInspection& inspection);

bool isAuthoritativeCommitted(PageEvidence evidence);

// Pure two-page evidence helper for the architecture gate: two authoritative
// committed records with different non-zero incarnations are contradictory.
bool conflictingCommittedIncarnations(const PageInspection& first,
                                      const PageInspection& second);

}  // namespace geofence_format
}  // namespace orun_tlp
