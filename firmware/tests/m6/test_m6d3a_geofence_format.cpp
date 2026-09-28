// M6D3A: exact geofence snapshot bytes, canonicalization and page evidence.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "geofence_format.h"
#include "journal_format.h"

using namespace orun_tlp;
using namespace orun_tlp::geofence_format;

namespace {

void resealBody(uint8_t* bytes) {
  journal_format::put32(bytes + kCrcOffset,
                        journal_format::crc32(bytes, kCrcOffset));
}

PageInspection inspect(const uint8_t* bytes) {
  PageInspection result;
  assert(inspectPage(bytes, kRecordSize, result));
  return result;
}

Record makeRecord(const Snapshot& snapshot, uint64_t generation,
                  uint64_t incarnation, uint32_t revision) {
  Record record;
  record.generation = generation;
  record.token = StateToken(incarnation, revision);
  record.snapshot = snapshot;
  return record;
}

Snapshot oneTriangle(bool explicit_close) {
  const GeoPointE7 vertices[] = {
      GeoPointE7(10000000, 20000000),
      GeoPointE7(10010000, 20000000),
      GeoPointE7(10000000, 20010000),
      GeoPointE7(10000000, 20000000),
  };
  const GeofencePolygonView polygon(vertices, explicit_close ? 4 : 3);
  Snapshot snapshot;
  assert(canonicalizeConfiguredAreaSet(GeofenceAreaSetView(&polygon, 1),
                                       snapshot));
  return snapshot;
}

Snapshot maximumSnapshot() {
  GeoPointE7 vertices[8][8];
  GeofencePolygonView areas[8];
  const int8_t shape[8][2] = {
      {-2, -1}, {-1, -2}, {1, -2}, {2, -1},
      {2, 1}, {1, 2}, {-1, 2}, {-2, 1},
  };
  for (uint8_t area = 0; area < 8; ++area) {
    const int32_t base_lat = 10000000 + static_cast<int32_t>(area) * 100000;
    const int32_t base_lon = 20000000 + static_cast<int32_t>(area) * 100000;
    for (uint8_t vertex = 0; vertex < 8; ++vertex) {
      vertices[area][vertex] = GeoPointE7(
          base_lat + static_cast<int32_t>(shape[vertex][0]) * 1000,
          base_lon + static_cast<int32_t>(shape[vertex][1]) * 1000);
    }
    areas[area] = GeofencePolygonView(vertices[area], 8);
  }
  Snapshot snapshot;
  assert(canonicalizeConfiguredAreaSet(GeofenceAreaSetView(areas, 8), snapshot));
  return snapshot;
}

}  // namespace

int main() {
  static_assert(kMaximumAreas == 8, "M6D3 production area capacity");
  static_assert(kMaximumTotalVertices == 64, "M6D3 production vertex capacity");
  static_assert(kPayloadSize == 520, "fixed v1 payload");
  static_assert(kCrcOffset == 556, "fixed v1 CRC offset");
  static_assert(kCommitOffset == 560, "fixed v1 commit offset");
  static_assert(kRecordSize == 564, "fixed v1 record size");

  // Exact CLEAR golden header + whole-body CRC.
  {
    Snapshot clear;
    makeClearSnapshot(clear);
    const Record record = makeRecord(
        clear, 0x0102030405060708ULL, 0x1112131415161718ULL, 0x21222324UL);
    uint8_t bytes[kRecordSize];
    assert(encode(record, bytes, sizeof(bytes)));

    const uint8_t expected_header[kVerticesOffset] = {
        0x4F, 0x52, 0x47, 0x31, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    assert(memcmp(bytes, expected_header, sizeof(expected_header)) == 0);
    for (uint32_t i = kVerticesOffset; i < kCrcOffset; ++i)
      assert(bytes[i] == 0);
    assert(journal_format::get32(bytes + kCrcOffset) == 0x79FBEF2BUL);
    assert(journal_format::get32(bytes + kCommitOffset) == kCommit);

    Record decoded;
    assert(decode(bytes, sizeof(bytes), decoded));
    assert(decoded.generation == record.generation);
    assert(decoded.token.incarnation == record.token.incarnation);
    assert(decoded.token.revision == record.token.revision);
    assert(decoded.snapshot.state == ResourceState::kClear);
    assert(inspect(bytes).evidence == PageEvidence::kCommittedClear);
  }

  // Maximum 8-area / 64-vertex golden. The hard-coded CRC covers every fixed
  // header, count and coordinate byte and therefore guards the exact layout.
  {
    const Snapshot snapshot = maximumSnapshot();
    assert(snapshot.area_count == 8);
    assert(snapshot.total_vertex_count == 64);
    for (uint8_t i = 0; i < 8; ++i) assert(snapshot.area_vertex_counts[i] == 8);

    const Record record =
        makeRecord(snapshot, 9, 0xABCDEF0102030405ULL, 7);
    uint8_t bytes[kRecordSize];
    assert(encode(record, bytes, sizeof(bytes)));
    assert(bytes[kStateOffset] == static_cast<uint8_t>(ResourceState::kConfigured));
    assert(bytes[kAreaCountOffset] == 8);
    assert(journal_format::get16(bytes + kTotalVertexCountOffset) == 64);
    assert(journal_format::get32(bytes + kCrcOffset) == 0xE9DCD019UL);

    Record decoded;
    assert(decode(bytes, sizeof(bytes), decoded));
    assert(decoded.snapshot.total_vertex_count == 64);
    assert(decoded.snapshot.vertices[0].latitude_e7 == 9998000);
    assert(decoded.snapshot.vertices[63].longitude_e7 == 20601000);
    assert(inspect(bytes).evidence == PageEvidence::kCommittedConfigured);
  }

  // Explicit closing duplicate canonicalizes away. Equivalent canonical input
  // produces byte-identical persistence without polygon sorting/reorientation.
  {
    const Snapshot implicit = oneTriangle(false);
    const Snapshot explicit_close = oneTriangle(true);
    assert(implicit.total_vertex_count == 3);
    assert(explicit_close.total_vertex_count == 3);

    uint8_t a[kRecordSize], b[kRecordSize];
    assert(encode(makeRecord(implicit, 3, 99, 2), a, sizeof(a)));
    assert(encode(makeRecord(explicit_close, 3, 99, 2), b, sizeof(b)));
    assert(memcmp(a, b, sizeof(a)) == 0);
  }

  // Physical generation is independent from semantic revision.
  {
    const Snapshot snapshot = oneTriangle(false);
    uint8_t first[kRecordSize], second[kRecordSize];
    assert(encode(makeRecord(snapshot, 10, 77, 5), first, sizeof(first)));
    assert(encode(makeRecord(snapshot, 11, 77, 5), second, sizeof(second)));
    Record a, b;
    assert(decode(first, sizeof(first), a));
    assert(decode(second, sizeof(second), b));
    assert(a.generation == 10 && b.generation == 11);
    assert(a.token.revision == 5 && b.token.revision == 5);
  }

  // Staged body is semantically decodable but not authoritative until commit.
  {
    uint8_t bytes[kRecordSize];
    assert(encode(makeRecord(oneTriangle(false), 4, 55, 2), bytes, sizeof(bytes)));
    memset(bytes + kCommitOffset, 0xFF, 4);
    Record decoded;
    assert(decodeBody(bytes, sizeof(bytes), decoded));
    assert(!decode(bytes, sizeof(bytes), decoded));
    const PageInspection result = inspect(bytes);
    assert(result.evidence == PageEvidence::kStaged);
    assert(result.has_decoded_record);
    assert(!isAuthoritativeCommitted(result.evidence));
  }

  // Partial commit stays non-authoritative even with a verified body.
  {
    uint8_t bytes[kRecordSize];
    assert(encode(makeRecord(oneTriangle(false), 4, 55, 2), bytes, sizeof(bytes)));
    journal_format::put32(bytes + kCommitOffset, 0x00FFFFFFUL);
    const PageInspection result = inspect(bytes);
    assert(result.evidence == PageEvidence::kPartialCommit);
    assert(result.has_decoded_record);
    assert(!isAuthoritativeCommitted(result.evidence));
  }

  // Structural/semantic fields are checked even when CRC is deliberately
  // resealed so each rejection is not merely a checksum failure.
  {
    uint8_t good[kRecordSize];
    assert(encode(makeRecord(oneTriangle(false), 4, 55, 2), good, sizeof(good)));
    Record decoded;

    uint8_t bytes[kRecordSize];
    memcpy(bytes, good, sizeof(bytes));
    bytes[5] = 1;
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    bytes[34] = 1;
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    journal_format::put16(bytes + kPayloadSizeOffset, kPayloadSize - 4);
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    bytes[kAreaCountOffset] = 9;
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    journal_format::put16(bytes + kTotalVertexCountOffset, 65);
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    bytes[kAreaVertexCountsOffset] = 2;
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    bytes[kAreaCountOffset] = 2;
    bytes[kAreaVertexCountsOffset] = 64;
    bytes[kAreaVertexCountsOffset + 1] = 64;
    journal_format::put16(bytes + kTotalVertexCountOffset, 64);
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));

    memcpy(bytes, good, sizeof(bytes));
    journal_format::put32(bytes + kVerticesOffset, 900000000UL);
    resealBody(bytes);
    assert(!decode(bytes, sizeof(bytes), decoded));
  }

  // Plain CRC corruption with commit present is committed-corrupt.
  {
    uint8_t bytes[kRecordSize];
    assert(encode(makeRecord(oneTriangle(false), 4, 55, 2), bytes, sizeof(bytes)));
    bytes[kVerticesOffset + 3] ^= 1;
    assert(inspect(bytes).evidence == PageEvidence::kCommittedCorrupt);
  }

  // Fully erased is blank evidence, never authoritative CLEAR.
  {
    uint8_t bytes[kRecordSize];
    memset(bytes, 0xFF, sizeof(bytes));
    const PageInspection result = inspect(bytes);
    assert(result.evidence == PageEvidence::kErased);
    assert(!result.has_decoded_record);
    assert(!isAuthoritativeCommitted(result.evidence));
  }

  // Reserved future schema is unsupported/non-destructive; a prefix-only
  // partial program takes torn/corrupt precedence rather than masquerading as
  // a complete future schema.
  {
    uint8_t bytes[kRecordSize];
    memset(bytes, 0, sizeof(bytes));
    journal_format::put32(bytes, kMagic);
    bytes[4] = 4;
    assert(inspect(bytes).evidence == PageEvidence::kUnsupportedNewer);

    memset(bytes, 0xFF, sizeof(bytes));
    journal_format::put32(bytes, kMagic);
    bytes[4] = 4;
    bytes[5] = bytes[6] = bytes[7] = 0;
    assert(inspect(bytes).evidence == PageEvidence::kSupportedCorrupt);
  }

  // Two authoritative pages from different token incarnations are explicitly
  // contradictory authority. Same incarnation is not this specific conflict.
  {
    uint8_t first_bytes[kRecordSize], second_bytes[kRecordSize];
    const Snapshot snapshot = oneTriangle(false);
    assert(encode(makeRecord(snapshot, 7, 1001, 4), first_bytes, sizeof(first_bytes)));
    assert(encode(makeRecord(snapshot, 8, 2002, 1), second_bytes, sizeof(second_bytes)));
    const PageInspection first = inspect(first_bytes);
    const PageInspection second = inspect(second_bytes);
    assert(conflictingCommittedIncarnations(first, second));

    assert(encode(makeRecord(snapshot, 8, 1001, 5), second_bytes, sizeof(second_bytes)));
    assert(!conflictingCommittedIncarnations(first, inspect(second_bytes)));
  }

  // Encoder rejects invalid authority before touching output.
  {
    Record bad = makeRecord(oneTriangle(false), 0, 7, 1);
    uint8_t bytes[kRecordSize];
    memset(bytes, 0xA5, sizeof(bytes));
    assert(!encode(bad, bytes, sizeof(bytes)));
    for (uint32_t i = 0; i < kRecordSize; ++i) assert(bytes[i] == 0xA5);

    bad = makeRecord(oneTriangle(false), 1, 0, 1);
    assert(!encode(bad, bytes, sizeof(bytes)));
    bad = makeRecord(oneTriangle(false), 1, 7, 0);
    assert(!encode(bad, bytes, sizeof(bytes)));
  }

  // Invalid arguments do not fabricate evidence.
  {
    uint8_t bytes[kRecordSize];
    memset(bytes, 0xFF, sizeof(bytes));
    PageInspection inspection;
    assert(!inspectPage(nullptr, sizeof(bytes), inspection));
    assert(!inspectPage(bytes, kRecordSize - 1, inspection));
    Record record;
    assert(!decode(nullptr, sizeof(bytes), record));
    assert(!decode(bytes, kRecordSize - 1, record));
  }

  puts("M6D3A geofence format/classifier checks: PASS");
}
