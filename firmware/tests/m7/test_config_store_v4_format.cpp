// ConfigStore schema v4: exact bytes, decode rules and page evidence.
// docs/architecture/ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md sections 3, 4, 8.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_format.h"
#include "journal_format.h"

using namespace orun_tlp::config_format;
namespace jf = orun_tlp::journal_format;

namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// Recompute the CRC after a deliberate field change, so a decode failure is
// caused by the field rule and not by the CRC.
void reseal(uint8_t* bytes) {
  put32(bytes + kV4CrcOffset, jf::crc32(bytes, kV4CrcOffset));
}

PageInspection inspect(const uint8_t* page, size_t size = kV4PagePrefixSize) {
  PageInspection result;
  assert(inspectPagePrefix(page, size, result));
  return result;
}

void makeV4Page(const TokenizedRecord& record, uint8_t* page) {
  memset(page, 0xFF, kV4PagePrefixSize);
  encodeV4(record, page);
}

const TokenizedRecord kRecord(
    7, Config(600, 1200, kServiceModeExplicit,
              kServiceTracking | kServiceRelayForwarding),
    StateToken(0x1122334455667788ULL, 9));

// The page classifier of firmware that knows only schema v2, frozen from
// main@7782a55 (src/config_format.cpp inspectPagePrefix). It answers one
// question here: does an older device treat a v4 page as a newer schema it
// must leave alone? Only the UNSUPPORTED_NEWER outcome is modelled exactly.
namespace v2_only {
bool wordErased(const uint8_t* b) {
  return b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF;
}
bool programmedPrefixWithErasedTail(const uint8_t* bytes, size_t size) {
  bool saw_programmed = false, saw_erased_tail = false;
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
bool seesUnsupportedNewer(const uint8_t* bytes) {
  if (jf::erased(bytes, kV2PagePrefixSize)) return false;
  const bool magic = jf::get32(bytes) == kMagic;
  const uint8_t version = bytes[4];
  if (magic && (version == kVersion || version == kV2Version)) return false;
  if (programmedPrefixWithErasedTail(bytes, kV2PagePrefixSize)) return false;
  if (!magic || version == kVersion || version == kV2Version ||
      version == 0xFF)
    return false;
  return bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
         (version & 0x03U) == 0;
}
}  // namespace v2_only

}  // namespace

int main() {
  static_assert(kV4Version == 4, "v4 version");
  static_assert(kV4RecordSize == 68, "v4 sealed record must be 68 bytes");
  static_assert(kV4CrcOffset == 60, "v4 CRC offset");
  static_assert(kV4CommitOffset == 64, "v4 commit offset");
  static_assert(kV4RetireOffset == 68, "v4 retire offset");
  static_assert(kV4PagePrefixSize == 72, "v4 page-local prefix size");
  static_assert(kV4BodyAndCrcSize == 64, "v4 body+CRC is the staging size");
  static_assert(kWriteRecordSize == kV4RecordSize, "saves write v4");

  // Golden bytes, computed independently (Python zlib.crc32 over [0..59]).
  {
    const TokenizedRecord record(
        0x0102030405060708ULL,
        Config(0x11223344UL, 0x55667788UL, kServiceModeExplicit,
               kServiceTracking | kServiceApplicationReceive),
        StateToken(0x0102030405060709ULL, 0x0A0B0C0DUL));
    uint8_t bytes[kV4RecordSize];
    encodeV4(record, bytes);
    const uint8_t expected[kV4RecordSize] = {
        0x4F, 0x52, 0x43, 0x31, 0x04, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x00, 0x28, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0x01, 0x05, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09,
        0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xBE, 0xD9, 0xCC, 0xF2,
        0x00, 0x00, 0x00, 0x00,
    };
    assert(memcmp(bytes, expected, sizeof(expected)) == 0);

    TokenizedRecord decoded;
    assert(decodeV4(bytes, decoded));
    assert(decoded.generation == record.generation);
    assert(decoded.config.tracking_interval_seconds == 0x11223344UL);
    assert(decoded.config.battery_capacity_mah == 0x55667788UL);
    assert(decoded.config.service_mode == kServiceModeExplicit);
    assert(decoded.config.requested_services == 0x05);
    assert(decoded.token.incarnation == 0x0102030405060709ULL);
    assert(decoded.token.revision == 0x0A0B0C0DUL);

    // A v4 record is never a v2 record and vice versa.
    V2Record as_v2;
    assert(!decodeV2(bytes, as_v2));
    uint8_t v2[kV2RecordSize];
    encodeV2(V2Record(1, Config(180, 0), StateToken(1, 1)), v2);
    TokenizedRecord as_v4;
    assert(!decodeV4Body(v2, as_v4));
  }

  // Commit state: body validity is independent of the commit word.
  {
    uint8_t bytes[kV4RecordSize];
    encodeV4(kRecord, bytes);
    memset(bytes + kV4CommitOffset, 0xFF, 4);
    TokenizedRecord decoded;
    assert(decodeV4Body(bytes, decoded));
    assert(!decodeV4(bytes, decoded));
    assert(!decodeV4(nullptr, decoded) && !decodeV4Body(nullptr, decoded));
  }

  // Every structural rule rejects on its own (CRC recomputed each time).
  {
    struct Mutation {
      uint32_t offset;
      uint8_t value;
    };
    const Mutation mutations[] = {
        {4, 2},     {4, 8},                   // other schema versions
        {5, 1},     {6, 1},   {7, 1},         // reserved after version
        {17, 39},   {16, 1},                  // payload length
        {18, 1},    {19, 1},                  // reserved after length
        {30, 1},    {31, 1},                  // reserved after services
        {44, 1},    {48, 1},  {51, 1}, {59, 1},  // reserved room
    };
    for (const Mutation& m : mutations) {
      uint8_t bytes[kV4RecordSize];
      encodeV4(kRecord, bytes);
      bytes[m.offset] = m.value;
      reseal(bytes);
      TokenizedRecord decoded;
      assert(!decodeV4Body(bytes, decoded));
    }
    // Zero generation, incarnation or revision.
    const uint32_t zero_fields[][2] = {{8, 8}, {32, 8}, {40, 4}};
    for (const auto& field : zero_fields) {
      uint8_t bytes[kV4RecordSize];
      encodeV4(kRecord, bytes);
      memset(bytes + field[0], 0, field[1]);
      reseal(bytes);
      TokenizedRecord decoded;
      assert(!decodeV4Body(bytes, decoded));
    }
    // Wrong magic; CRC mismatch.
    {
      uint8_t bytes[kV4RecordSize];
      encodeV4(kRecord, bytes);
      bytes[0] = 0x50;
      reseal(bytes);
      TokenizedRecord decoded;
      assert(!decodeV4Body(bytes, decoded));
      encodeV4(kRecord, bytes);
      bytes[21] ^= 0x01;
      assert(!decodeV4Body(bytes, decoded));
    }
    // Service mode/bits are NOT structural: an unknown value decodes, so the
    // store can classify it as semantic corruption with the evidence intact.
    {
      uint8_t bytes[kV4RecordSize];
      encodeV4(kRecord, bytes);
      bytes[28] = 7;
      bytes[29] = 0xF8;
      reseal(bytes);
      TokenizedRecord decoded;
      assert(decodeV4Body(bytes, decoded));
      assert(decoded.config.service_mode == 7);
      assert(decoded.config.requested_services == 0xF8);
    }
  }

  // Page evidence, mirroring the v2 classifier cases.
  {
    uint8_t page[kV4PagePrefixSize];

    makeV4Page(kRecord, page);
    PageInspection committed = inspect(page);
    assert(committed.evidence == PageEvidence::kV4Committed);
    assert(committed.has_decoded_record);
    assert(committed.generation == 7);
    assert(committed.config.requested_services ==
           (kServiceTracking | kServiceRelayForwarding));
    assert(committed.token.revision == 9);
    assert(evidenceIsCommitted(committed.evidence));
    assert(ownedPrefixSize(committed.evidence) == kV4PagePrefixSize);

    makeV4Page(kRecord, page);
    page[kV4RetireOffset + 3] = 0x00;
    assert(inspect(page).evidence == PageEvidence::kV4CommittedRetired);
    assert(evidenceIsCommittedRetired(PageEvidence::kV4CommittedRetired));

    makeV4Page(kRecord, page);
    memset(page + kV4CommitOffset, 0xFF, 4);
    PageInspection staged = inspect(page);
    assert(staged.evidence == PageEvidence::kV4Staged);
    assert(staged.has_decoded_record);
    assert(evidenceIsStaged(staged.evidence));

    makeV4Page(kRecord, page);
    page[kV4CommitOffset] = 0x00;
    page[kV4CommitOffset + 1] = 0xFF;
    page[kV4CommitOffset + 2] = 0xFF;
    page[kV4CommitOffset + 3] = 0xFF;
    assert(inspect(page).evidence == PageEvidence::kV4PartialCommit);
    assert(evidenceIsPartialCommit(PageEvidence::kV4PartialCommit));

    makeV4Page(kRecord, page);
    page[22] ^= 0x01;
    assert(inspect(page).evidence == PageEvidence::kV4CommittedCorrupt);

    // Torn body: header landed, the rest of the body and commit did not.
    makeV4Page(kRecord, page);
    memset(page + 20, 0xFF, kV4PagePrefixSize - 20);
    const PageInspection torn = inspect(page);
    assert(torn.evidence == PageEvidence::kV4UncommittedOrTorn);
    assert(!torn.has_decoded_record);
    assert(evidenceIsUncommittedOrTorn(torn.evidence));

    // Invalid body with a garbage commit word: supported corruption.
    makeV4Page(kRecord, page);
    page[22] ^= 0x01;
    page[kV4CommitOffset] = 0x12;
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);

    // A v4 page needs its full 72-byte prefix to be classified.
    makeV4Page(kRecord, page);
    assert(inspect(page, kV2PagePrefixSize).evidence ==
           PageEvidence::kSupportedCorrupt);

    // Erased now means all 72 owned bytes; a programmed byte at 60 is not
    // "erased", whatever the first 52 bytes say.
    memset(page, 0xFF, sizeof(page));
    assert(inspect(page).evidence == PageEvidence::kErased);
    page[60] = 0x00;
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);

    // v2 evidence keeps its own owned prefix.
    assert(ownedPrefixSize(PageEvidence::kV2Committed) == kV2PagePrefixSize);
    assert(ownedPrefixSize(PageEvidence::kErased) == kV2PagePrefixSize);
    assert(evidenceIsCommitted(PageEvidence::kV2Committed));
    assert(!evidenceIsCommitted(PageEvidence::kV4CommittedCorrupt));
  }

  // The next reserved version stays UNSUPPORTED_NEWER for v4 firmware too.
  {
    uint8_t page[kV4PagePrefixSize];
    memset(page, 0, sizeof(page));
    put32(page, kMagic);
    page[4] = 8;
    assert(inspect(page).evidence == PageEvidence::kUnsupportedNewer);
  }

  // Downgrade boundary (section 6): firmware that knows only v2 classifies
  // every committed v4 page -- with or without a retire word, any field
  // values -- as UNSUPPORTED_NEWER, so it neither erases nor reinterprets it.
  {
    const uint8_t modes[] = {kServiceModeAuto, kServiceModeExplicit};
    const uint32_t intervals[] = {1, 60, 180, 0xFFFFFFFFUL};
    for (const uint8_t mode : modes) {
      for (uint8_t services = 0; services <= kKnownServicesMask; ++services) {
        if (mode == kServiceModeAuto && services != 0) continue;
        for (const uint32_t interval : intervals) {
          for (int retired = 0; retired < 2; ++retired) {
            uint8_t page[kV4PagePrefixSize];
            makeV4Page(TokenizedRecord(UINT64_MAX - 1,
                                       Config(interval, 0xFFFFFFFFUL, mode,
                                              services),
                                       StateToken(UINT64_MAX, UINT32_MAX)),
                       page);
            if (retired) page[kV4RetireOffset] = 0x00;
            assert(v2_only::seesUnsupportedNewer(page));
            // and this firmware classifies the same bytes exactly.
            assert(evidenceIsCommitted(inspect(page).evidence) ||
                   evidenceIsCommittedRetired(inspect(page).evidence));
          }
        }
      }
    }
    // The discriminator really depends on bytes 48..51 being programmed:
    // the same header with those bytes erased reads as a torn write.
    uint8_t page[kV4PagePrefixSize];
    makeV4Page(kRecord, page);
    memset(page + 48, 0xFF, kV4PagePrefixSize - 48);
    assert(!v2_only::seesUnsupportedNewer(page));
    // and a v2 page is still a v2 page to the old classifier.
    uint8_t v2[kV4PagePrefixSize];
    memset(v2, 0xFF, sizeof(v2));
    encodeV2(V2Record(1, Config(180, 0), StateToken(1, 1)), v2);
    assert(!v2_only::seesUnsupportedNewer(v2));
  }

  puts("ConfigStore v4 format/classifier checks: PASS");
}
