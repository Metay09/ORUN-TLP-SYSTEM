// ConfigStore v2 exact byte layout and low-level page-evidence classifier.
//
// This is deliberately a pure format/evidence test. The production ConfigStore
// still uses legacy v1 in this slice; semantic candidate policy and the clean
// development reset -> fresh-v2 runtime cutover are tested/implemented later.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_format.h"

using namespace orun_tlp::config_format;

namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

PageInspection inspect(const uint8_t* page) {
  PageInspection result;
  assert(inspectPagePrefix(page, kV2PagePrefixSize, result));
  return result;
}

void makeV2Page(const V2Record& record, uint8_t* page) {
  memset(page, 0xFF, kV2PagePrefixSize);
  encodeV2(record, page);
}

void assertConfig(const Config& config, uint32_t interval, uint32_t battery) {
  assert(config.tracking_interval_seconds == interval);
  assert(config.battery_capacity_mah == battery);
}

}  // namespace

int main() {
  static_assert(kVersion == 1, "legacy v1 version changed");
  static_assert(kRecordSize == 36, "legacy v1 record size changed");
  static_assert(kV2RecordSize == 48, "v2 sealed record must be 48 bytes");
  static_assert(kV2CrcOffset == 40, "v2 CRC offset");
  static_assert(kV2CommitOffset == 44, "v2 commit offset");
  static_assert(kV2RetireOffset == 48, "v2 retire offset");
  static_assert(kV2PagePrefixSize == 52, "v2 page-local prefix size");

  // Exact golden bytes: proves field offsets, big-endian encoding, CRC span
  // bytes[0..39], commit offset 44 and 48-byte sealed-record size.
  {
    const V2Record record(
        0x0102030405060708ULL,
        Config(0x11223344UL, 0x55667788UL),
        StateToken(0x0102030405060709ULL, 0x0A0B0C0DUL));
    uint8_t bytes[kV2RecordSize];
    encodeV2(record, bytes);

    const uint8_t expected[kV2RecordSize] = {
        0x4F, 0x52, 0x43, 0x31, 0x02, 0x00, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x00, 0x14, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0xC4, 0x59, 0x60, 0x94, 0x00, 0x00, 0x00, 0x00,
    };
    assert(memcmp(bytes, expected, sizeof(expected)) == 0);

    V2Record decoded;
    assert(decodeV2(bytes, decoded));
    assert(decoded.generation == record.generation);
    assertConfig(decoded.config, 0x11223344UL, 0x55667788UL);
    assert(decoded.token.incarnation == 0x0102030405060709ULL);
    assert(decoded.token.revision == 0x0A0B0C0DUL);
  }

  // decodeV2Body accepts a verified staged body independently of commit;
  // decodeV2 still requires the committed marker.
  {
    uint8_t bytes[kV2RecordSize];
    encodeV2(V2Record(7, Config(247, 12000), StateToken(9, 3)), bytes);
    memset(bytes + kV2CommitOffset, 0xFF, 4);

    V2Record decoded;
    assert(decodeV2Body(bytes, decoded));
    assert(!decodeV2(bytes, decoded));
    assert(decoded.generation == 7);
    assertConfig(decoded.config, 247, 12000);
    assert(decoded.token.incarnation == 9);
    assert(decoded.token.revision == 3);
  }

  // Structural v2 rejection: generation/incarnation/revision must be nonzero,
  // reserved/length/version/CRC must match the exact contract.
  {
    const V2Record good(1, Config(180, 0), StateToken(2, 1));
    uint8_t bytes[kV2RecordSize];
    V2Record decoded;

    encodeV2(V2Record(0, good.config, good.token), bytes);
    assert(!decodeV2(bytes, decoded));

    encodeV2(V2Record(1, good.config, StateToken(0, 1)), bytes);
    assert(!decodeV2(bytes, decoded));

    encodeV2(V2Record(1, good.config, StateToken(2, 0)), bytes);
    assert(!decodeV2(bytes, decoded));

    encodeV2(good, bytes);
    bytes[5] = 1;
    assert(!decodeV2(bytes, decoded));

    encodeV2(good, bytes);
    bytes[17] ^= 0x01;
    assert(!decodeV2(bytes, decoded));

    encodeV2(good, bytes);
    bytes[20] ^= 0x01;
    assert(!decodeV2(bytes, decoded));

    encodeV2(good, bytes);
    bytes[4] = 4;
    assert(!decodeV2(bytes, decoded));
  }

  // Fully erased page prefix.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    assert(inspect(page).evidence == PageEvidence::kErased);
  }

  // Legacy v1 committed bytes are recognized only so a future v2 cutover can
  // diagnose/reset an old development partition. This is not migration support.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    encode(Config(247, 9000), 7, page);

    const PageInspection result = inspect(page);
    assert(result.evidence == PageEvidence::kLegacyV1Committed);
    assert(result.has_decoded_record);
    assert(result.generation == 7);
    assertConfig(result.config, 247, 9000);
  }

  // v1 commit erased is non-authoritative even if the body was once valid.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    encode(Config(247, 9000), 7, page);
    memset(page + 32, 0xFF, 4);
    assert(inspect(page).evidence == PageEvidence::kLegacyV1UncommittedOrTorn);
  }

  // v1 committed structural corruption is kept distinct from an uncommitted
  // candidate and never becomes a future schema.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    encode(Config(247, 9000), 7, page);
    page[20] ^= 0x01;
    assert(inspect(page).evidence == PageEvidence::kLegacyV1CommittedCorrupt);
  }

  const V2Record v2(11, Config(300, 5000), StateToken(0x123456789ULL, 4));

  // Exact v2 committed / staged / torn-staged / retired classifications.
  {
    uint8_t page[kV2PagePrefixSize];
    makeV2Page(v2, page);

    PageInspection result = inspect(page);
    assert(result.evidence == PageEvidence::kV2Committed);
    assert(result.has_decoded_record);
    assert(result.generation == 11);
    assert(result.token.incarnation == 0x123456789ULL);
    assert(result.token.revision == 4);

    memset(page + kV2CommitOffset, 0xFF, 4);
    result = inspect(page);
    assert(result.evidence == PageEvidence::kV2Staged);
    assert(result.has_decoded_record);

    page[20] ^= 0x01;
    result = inspect(page);
    assert(result.evidence == PageEvidence::kV2UncommittedOrTorn);
    assert(!result.has_decoded_record);
  }

  // A verified body with a partial commit keeps semantic bytes recoverable but
  // never becomes authoritative token state.
  {
    uint8_t page[kV2PagePrefixSize];
    makeV2Page(v2, page);
    put32(page + kV2CommitOffset, 0x00FFFFFFUL);

    const PageInspection result = inspect(page);
    assert(result.evidence == PageEvidence::kV2PartialCommit);
    assert(result.has_decoded_record);
    assertConfig(result.config, 300, 5000);
    assert(result.token.incarnation == 0x123456789ULL);
    assert(result.token.revision == 4);
  }

  // A partial commit cannot rescue an invalid body.
  {
    uint8_t page[kV2PagePrefixSize];
    makeV2Page(v2, page);
    page[24] ^= 0x01;
    put32(page + kV2CommitOffset, 0x00FFFFFFUL);
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
  }

  // Any non-FF retire value retires an otherwise committed v2 token,
  // including a partially programmed retire word.
  {
    uint8_t page[kV2PagePrefixSize];
    makeV2Page(v2, page);
    put32(page + kV2RetireOffset, 0xFFFFFFFEUL);
    const PageInspection result = inspect(page);
    assert(result.evidence == PageEvidence::kV2CommittedRetired);
    assert(result.has_decoded_record);
  }

  // A committed v2 body with bad CRC/body is committed-corrupt.
  {
    uint8_t page[kV2PagePrefixSize];
    makeV2Page(v2, page);
    page[20] ^= 0x01;
    assert(inspect(page).evidence == PageEvidence::kV2CommittedCorrupt);
  }

  // R1 regression: half-erased v1/v2 version values cannot masquerade as
  // UNSUPPORTED_NEWER. Keep the page otherwise non-prefix so the version
  // discriminator itself is being exercised.
  {
    const uint8_t versions[] = {0x03, 0x05, 0x06, 0x0A};
    for (const uint8_t version : versions) {
      uint8_t page[kV2PagePrefixSize];
      memset(page, 0, sizeof(page));
      put32(page, kMagic);
      page[4] = version;
      assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
    }
  }

  // ORC1/version=FF outside a prefix-shaped torn write is local corruption.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0, sizeof(page));
    put32(page, kMagic);
    page[4] = 0xFF;
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
  }

  // A real reserved future version (multiple-of-four namespace, zero
  // reserved bytes) is unsupported and must not be auto-overwritten.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0, sizeof(page));
    put32(page, kMagic);
    page[4] = 4;
    assert(inspect(page).evidence == PageEvidence::kUnsupportedNewer);

    page[5] = 1;
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
  }

  // Classification precedence: even a numeric future-version byte is local
  // torn evidence when only a programmed prefix landed and the tail is erased.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    put32(page, kMagic);
    page[4] = 4;
    page[5] = page[6] = page[7] = 0;
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
  }

  // Random/damaged non-ORC1 bytes inside this exclusively owned partition are
  // supported local corruption, never automatically a future schema.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0, sizeof(page));
    put32(page, 0xDEADBEEFUL);
    assert(inspect(page).evidence == PageEvidence::kSupportedCorrupt);
  }

  // Invalid argument handling is explicit and does not fabricate evidence.
  {
    uint8_t page[kV2PagePrefixSize];
    memset(page, 0xFF, sizeof(page));
    PageInspection result;
    assert(!inspectPagePrefix(nullptr, sizeof(page), result));
    assert(!inspectPagePrefix(page, kV2PagePrefixSize - 1, result));
  }

  puts("ConfigStore v2 format/classifier checks: PASS");
}
