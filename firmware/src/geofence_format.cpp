#include "geofence_format.h"

#include <string.h>

#include "journal_format.h"

namespace orun_tlp {
namespace geofence_format {
namespace {

namespace jf = journal_format;

bool pointIsZero(const GeoPointE7& point) {
  return point.latitude_e7 == 0 && point.longitude_e7 == 0;
}

bool snapshotValid(const Snapshot& snapshot) {
  if (snapshot.state == ResourceState::kClear) {
    if (snapshot.area_count != 0 || snapshot.total_vertex_count != 0) return false;
    for (uint8_t i = 0; i < kMaximumAreas; ++i)
      if (snapshot.area_vertex_counts[i] != 0) return false;
    for (uint16_t i = 0; i < kMaximumTotalVertices; ++i)
      if (!pointIsZero(snapshot.vertices[i])) return false;
    return true;
  }

  if (snapshot.state != ResourceState::kConfigured || snapshot.area_count == 0 ||
      snapshot.area_count > kMaximumAreas || snapshot.total_vertex_count == 0 ||
      snapshot.total_vertex_count > kMaximumTotalVertices) {
    return false;
  }

  uint16_t sum = 0;
  uint16_t offset = 0;
  for (uint8_t area = 0; area < kMaximumAreas; ++area) {
    const uint8_t count = snapshot.area_vertex_counts[area];
    if (area >= snapshot.area_count) {
      if (count != 0) return false;
      continue;
    }
    if (count < 3 || count > geofence_config::kMaximumPolygonVertices) return false;
    if (sum > kMaximumTotalVertices - count) return false;
    sum = static_cast<uint16_t>(sum + count);
    const GeofencePolygonView polygon(snapshot.vertices + offset, count);
    // Durable v1 stores effective vertices only. The geometry layer accepts an
    // optional explicit closing duplicate for input convenience, but accepting
    // that duplicate here would create a second authoritative byte encoding for
    // the same canonical polygon.
    if (effectiveGeofenceVertexCount(polygon) != count) return false;
    if (validateGeofencePolygon(polygon) != GeofencePolygonValidation::kOk)
      return false;
    offset = static_cast<uint16_t>(offset + count);
  }
  if (sum != snapshot.total_vertex_count) return false;
  for (uint16_t i = snapshot.total_vertex_count; i < kMaximumTotalVertices; ++i)
    if (!pointIsZero(snapshot.vertices[i])) return false;
  return true;
}

bool recordValid(const Record& record) {
  return record.generation != 0 && record.token.incarnation != 0 &&
         record.token.revision != 0 && snapshotValid(record.snapshot);
}

bool wordErased(const uint8_t* bytes) {
  return bytes[0] == 0xFF && bytes[1] == 0xFF &&
         bytes[2] == 0xFF && bytes[3] == 0xFF;
}

bool programmedPrefixWithErasedTail(const uint8_t* bytes, size_t size) {
  if ((size & 3U) != 0) return false;
  bool saw_programmed = false;
  bool saw_erased_tail = false;
  for (size_t offset = 0; offset < size; offset += 4) {
    const bool erased = wordErased(bytes + offset);
    if (erased) {
      if (saw_programmed) saw_erased_tail = true;
      continue;
    }
    if (saw_erased_tail) return false;
    saw_programmed = true;
  }
  return saw_programmed && saw_erased_tail;
}

bool unsupportedNewerDiscriminator(const uint8_t* bytes) {
  if (jf::get32(bytes) != kMagic) return false;
  const uint8_t version = bytes[4];
  if (version == kVersion || version == 0xFF) return false;
  return bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
         (version & 0x03U) == 0;
}

void setDecoded(const Record& record, PageInspection& inspection) {
  inspection.has_decoded_record = true;
  inspection.record = record;
}

}  // namespace

Snapshot::Snapshot()
    : state(ResourceState::kClear), area_count(0), total_vertex_count(0) {
  memset(area_vertex_counts, 0, sizeof(area_vertex_counts));
  for (uint16_t i = 0; i < kMaximumTotalVertices; ++i)
    vertices[i] = GeoPointE7();
}

Record::Record() : generation(0), token(), snapshot() {}

PageInspection::PageInspection()
    : evidence(PageEvidence::kSupportedCorrupt),
      has_decoded_record(false), record() {}

void makeClearSnapshot(Snapshot& snapshot) {
  snapshot = Snapshot();
}

bool canonicalizeConfiguredAreaSet(const GeofenceAreaSetView& area_set,
                                   Snapshot& snapshot) {
  if (area_set.areas == nullptr || area_set.area_count == 0 ||
      area_set.area_count > kMaximumAreas) {
    return false;
  }

  Snapshot candidate;
  candidate.state = ResourceState::kConfigured;
  candidate.area_count = static_cast<uint8_t>(area_set.area_count);
  uint16_t total = 0;

  for (uint16_t area = 0; area < area_set.area_count; ++area) {
    const GeofencePolygonView& polygon = area_set.areas[area];
    if (validateGeofencePolygon(polygon) != GeofencePolygonValidation::kOk)
      return false;
    const uint16_t effective = effectiveGeofenceVertexCount(polygon);
    if (effective < 3 || effective > geofence_config::kMaximumPolygonVertices ||
        total > kMaximumTotalVertices - effective) {
      return false;
    }
    candidate.area_vertex_counts[area] = static_cast<uint8_t>(effective);
    for (uint16_t vertex = 0; vertex < effective; ++vertex)
      candidate.vertices[total + vertex] = polygon.vertices[vertex];
    total = static_cast<uint16_t>(total + effective);
  }

  candidate.total_vertex_count = total;
  if (!snapshotValid(candidate)) return false;
  snapshot = candidate;
  return true;
}

bool encode(const Record& record, uint8_t* bytes, size_t size) {
  if (bytes == nullptr || size < kRecordSize || !recordValid(record)) return false;

  memset(bytes, 0, kRecordSize);
  jf::put32(bytes, kMagic);
  bytes[4] = kVersion;
  jf::put64(bytes + 8, record.generation);
  jf::put64(bytes + 16, record.token.incarnation);
  jf::put32(bytes + 24, record.token.revision);
  bytes[kStateOffset] = static_cast<uint8_t>(record.snapshot.state);
  bytes[kAreaCountOffset] = record.snapshot.area_count;
  jf::put16(bytes + kTotalVertexCountOffset, record.snapshot.total_vertex_count);
  jf::put16(bytes + kPayloadSizeOffset, kPayloadSize);

  for (uint8_t area = 0; area < kMaximumAreas; ++area)
    bytes[kAreaVertexCountsOffset + area] = record.snapshot.area_vertex_counts[area];

  uint32_t offset = kVerticesOffset;
  for (uint16_t vertex = 0; vertex < kMaximumTotalVertices; ++vertex) {
    jf::put32(bytes + offset,
              static_cast<uint32_t>(record.snapshot.vertices[vertex].latitude_e7));
    jf::put32(bytes + offset + 4,
              static_cast<uint32_t>(record.snapshot.vertices[vertex].longitude_e7));
    offset += 8;
  }

  jf::put32(bytes + kCrcOffset, jf::crc32(bytes, kCrcOffset));
  jf::put32(bytes + kCommitOffset, kCommit);
  return true;
}

bool decodeBody(const uint8_t* bytes, size_t size, Record& record) {
  if (bytes == nullptr || size < kRecordSize) return false;
  if (jf::get32(bytes) != kMagic || bytes[4] != kVersion ||
      bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0) {
    return false;
  }
  if (bytes[34] != 0 || bytes[35] != 0) return false;
  if (jf::get16(bytes + kPayloadSizeOffset) != kPayloadSize) return false;
  if (jf::get32(bytes + kCrcOffset) != jf::crc32(bytes, kCrcOffset)) return false;

  Record candidate;
  candidate.generation = jf::get64(bytes + 8);
  candidate.token.incarnation = jf::get64(bytes + 16);
  candidate.token.revision = jf::get32(bytes + 24);
  if (candidate.generation == 0 || candidate.token.incarnation == 0 ||
      candidate.token.revision == 0) {
    return false;
  }

  const uint8_t state = bytes[kStateOffset];
  if (state > static_cast<uint8_t>(ResourceState::kConfigured)) return false;
  candidate.snapshot.state = static_cast<ResourceState>(state);
  candidate.snapshot.area_count = bytes[kAreaCountOffset];
  candidate.snapshot.total_vertex_count =
      jf::get16(bytes + kTotalVertexCountOffset);

  for (uint8_t area = 0; area < kMaximumAreas; ++area)
    candidate.snapshot.area_vertex_counts[area] =
        bytes[kAreaVertexCountsOffset + area];

  uint32_t offset = kVerticesOffset;
  for (uint16_t vertex = 0; vertex < kMaximumTotalVertices; ++vertex) {
    candidate.snapshot.vertices[vertex].latitude_e7 =
        static_cast<int32_t>(jf::get32(bytes + offset));
    candidate.snapshot.vertices[vertex].longitude_e7 =
        static_cast<int32_t>(jf::get32(bytes + offset + 4));
    offset += 8;
  }

  if (!recordValid(candidate)) return false;
  record = candidate;
  return true;
}

bool decode(const uint8_t* bytes, size_t size, Record& record) {
  if (bytes == nullptr || size < kRecordSize ||
      jf::get32(bytes + kCommitOffset) != kCommit) {
    return false;
  }
  return decodeBody(bytes, size, record);
}

bool inspectPage(const uint8_t* bytes, size_t size, PageInspection& inspection) {
  if (bytes == nullptr || size < kRecordSize) return false;
  inspection = PageInspection();

  if (jf::erased(bytes, kRecordSize)) {
    inspection.evidence = PageEvidence::kErased;
    return true;
  }

  const bool magic_matches = jf::get32(bytes) == kMagic;
  const uint8_t version = bytes[4];
  if (magic_matches && version == kVersion) {
    Record decoded;
    const bool body_valid = decodeBody(bytes, size, decoded);
    const uint32_t commit = jf::get32(bytes + kCommitOffset);

    if (commit == kErasedWord) {
      inspection.evidence = body_valid ? PageEvidence::kStaged
                                       : PageEvidence::kUncommittedOrTorn;
      if (body_valid) setDecoded(decoded, inspection);
      return true;
    }
    if (commit == kCommit) {
      if (!body_valid) {
        inspection.evidence = PageEvidence::kCommittedCorrupt;
        return true;
      }
      setDecoded(decoded, inspection);
      inspection.evidence =
          decoded.snapshot.state == ResourceState::kClear
              ? PageEvidence::kCommittedClear
              : PageEvidence::kCommittedConfigured;
      return true;
    }
    if (body_valid) {
      setDecoded(decoded, inspection);
      inspection.evidence = PageEvidence::kPartialCommit;
      return true;
    }
    inspection.evidence = PageEvidence::kSupportedCorrupt;
    return true;
  }

  if (programmedPrefixWithErasedTail(bytes, kRecordSize)) {
    inspection.evidence = PageEvidence::kSupportedCorrupt;
    return true;
  }
  if (unsupportedNewerDiscriminator(bytes)) {
    inspection.evidence = PageEvidence::kUnsupportedNewer;
    return true;
  }
  inspection.evidence = PageEvidence::kSupportedCorrupt;
  return true;
}

bool isAuthoritativeCommitted(PageEvidence evidence) {
  return evidence == PageEvidence::kCommittedClear ||
         evidence == PageEvidence::kCommittedConfigured;
}

bool conflictingCommittedIncarnations(const PageInspection& first,
                                      const PageInspection& second) {
  return isAuthoritativeCommitted(first.evidence) &&
         isAuthoritativeCommitted(second.evidence) &&
         first.has_decoded_record && second.has_decoded_record &&
         first.record.token.incarnation != second.record.token.incarnation;
}

}  // namespace geofence_format
}  // namespace orun_tlp
