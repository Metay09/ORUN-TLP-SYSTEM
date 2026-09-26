#include "config_format.h"
#include <string.h>
#include "journal_format.h"

namespace orun_tlp::config_format {
namespace jf = orun_tlp::journal_format;
namespace {

// Legacy v1 offsets are frozen by M7P5.
constexpr unsigned kV1CrcOffset = 28;
constexpr unsigned kV1CommitOffset = 32;

void sealV1(uint8_t* bytes) {
  jf::put32(bytes + kV1CrcOffset, jf::crc32(bytes, kV1CrcOffset));
  jf::put32(bytes + kV1CommitOffset, kCommit);
}

bool sealedV1(const uint8_t* bytes) {
  return jf::get32(bytes + kV1CommitOffset) == kCommit &&
         jf::get32(bytes + kV1CrcOffset) ==
             jf::crc32(bytes, kV1CrcOffset);
}

bool validV2Body(const uint8_t* bytes, V2Record& record) {
  if (jf::get32(bytes) != kMagic) return false;
  if (bytes[4] != kV2Version || bytes[5] || bytes[6] || bytes[7])
    return false;

  const uint64_t generation = jf::get64(bytes + 8);
  if (generation == 0) return false;
  if (jf::get16(bytes + 16) != kV2PayloadSize ||
      jf::get16(bytes + 18) != 0)
    return false;

  if (jf::get32(bytes + kV2CrcOffset) !=
      jf::crc32(bytes, kV2CrcOffset))
    return false;

  const uint64_t incarnation = jf::get64(bytes + 28);
  const uint32_t revision = jf::get32(bytes + 36);
  if (incarnation == 0 || revision == 0) return false;

  V2Record decoded(
      generation,
      Config(jf::get32(bytes + 20), jf::get32(bytes + 24)),
      StateToken(incarnation, revision));
  record = decoded;
  return true;
}

bool wordErased(const uint8_t* bytes) {
  return bytes[0] == 0xFF && bytes[1] == 0xFF &&
         bytes[2] == 0xFF && bytes[3] == 0xFF;
}

// Nordic config writes are word aligned. Recognize the power-cut shape in
// which at least one leading word was programmed and all later words were
// never touched. Exact supported v1/v2 forms are classified before this
// helper is consulted.
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
  if (version == kVersion || version == kV2Version || version == 0xFF)
    return false;
  return bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
         (version & 0x03U) == 0;
}

void setV1Inspection(const uint8_t* bytes, PageInspection& inspection) {
  uint64_t generation = 0;
  Config config;
  if (decode(bytes, generation, config)) {
    inspection.evidence = PageEvidence::kV1Committed;
    inspection.has_decoded_record = true;
    inspection.generation = generation;
    inspection.config = config;
    return;
  }
  inspection.evidence = PageEvidence::kV1CommittedCorrupt;
}

void setV2DecodedInspection(const V2Record& record,
                            PageInspection& inspection) {
  inspection.has_decoded_record = true;
  inspection.generation = record.generation;
  inspection.config = record.config;
  inspection.token = record.token;
}

}  // namespace

void encode(const Config& config, uint64_t generation, uint8_t* bytes) {
  memset(bytes, 0, kRecordSize);
  jf::put32(bytes, kMagic);
  bytes[4] = kVersion;
  jf::put64(bytes + 8, generation);
  jf::put16(bytes + 16, kPayloadSize);
  jf::put32(bytes + 20, config.tracking_interval_seconds);
  jf::put32(bytes + 24, config.battery_capacity_mah);
  sealV1(bytes);
}

bool decode(const uint8_t* bytes, uint64_t& generation, Config& config) {
  if (jf::get32(bytes) != kMagic) return false;
  // bytes[4] is the version; bytes[5..7] are reserved and must be zero. An
  // unrecognized/newer version fails closed here -- it is never interpreted
  // as v1, satisfying the legacy v1 decoder contract.
  if (bytes[4] != kVersion || bytes[5] || bytes[6] || bytes[7]) return false;
  const uint64_t candidate_generation = jf::get64(bytes + 8);
  if (candidate_generation == 0) return false;
  if (jf::get16(bytes + 16) != kPayloadSize ||
      jf::get16(bytes + 18) != 0)
    return false;
  if (!sealedV1(bytes)) return false;
  generation = candidate_generation;
  config.tracking_interval_seconds = jf::get32(bytes + 20);
  config.battery_capacity_mah = jf::get32(bytes + 24);
  return true;
}

void encodeV2(const V2Record& record, uint8_t* bytes) {
  memset(bytes, 0, kV2RecordSize);
  jf::put32(bytes, kMagic);
  bytes[4] = kV2Version;
  jf::put64(bytes + 8, record.generation);
  jf::put16(bytes + 16, kV2PayloadSize);
  jf::put32(bytes + 20, record.config.tracking_interval_seconds);
  jf::put32(bytes + 24, record.config.battery_capacity_mah);
  jf::put64(bytes + 28, record.token.incarnation);
  jf::put32(bytes + 36, record.token.revision);
  jf::put32(bytes + kV2CrcOffset, jf::crc32(bytes, kV2CrcOffset));
  jf::put32(bytes + kV2CommitOffset, kCommit);
}

bool decodeV2Body(const uint8_t* bytes, V2Record& record) {
  if (bytes == nullptr) return false;
  V2Record decoded;
  if (!validV2Body(bytes, decoded)) return false;
  record = decoded;
  return true;
}

bool decodeV2(const uint8_t* bytes, V2Record& record) {
  if (bytes == nullptr ||
      jf::get32(bytes + kV2CommitOffset) != kCommit)
    return false;
  return decodeV2Body(bytes, record);
}

bool inspectPagePrefix(const uint8_t* bytes, size_t size,
                       PageInspection& inspection) {
  if (bytes == nullptr || size < kV2PagePrefixSize) return false;

  inspection = PageInspection();

  if (jf::erased(bytes, kV2PagePrefixSize)) {
    inspection.evidence = PageEvidence::kErased;
    return true;
  }

  const bool magic_matches = jf::get32(bytes) == kMagic;
  const uint8_t version = bytes[4];

  // Exact supported v1 is inspected before any generic torn/future rule.
  if (magic_matches && version == kVersion) {
    const uint32_t commit = jf::get32(bytes + kV1CommitOffset);
    if (commit == kErasedWord) {
      inspection.evidence = PageEvidence::kV1UncommittedOrTorn;
      return true;
    }
    if (commit == kCommit) {
      setV1Inspection(bytes, inspection);
      return true;
    }
    inspection.evidence = PageEvidence::kSupportedCorrupt;
    return true;
  }

  // Exact supported v2 likewise owns its known commit/retire offsets.
  if (magic_matches && version == kV2Version) {
    V2Record record;
    const bool body_valid = validV2Body(bytes, record);
    const uint32_t commit = jf::get32(bytes + kV2CommitOffset);

    if (commit == kErasedWord) {
      inspection.evidence = body_valid
          ? PageEvidence::kV2Staged
          : PageEvidence::kV2UncommittedOrTorn;
      if (body_valid) setV2DecodedInspection(record, inspection);
      return true;
    }

    if (commit == kCommit) {
      if (!body_valid) {
        inspection.evidence = PageEvidence::kV2CommittedCorrupt;
        return true;
      }
      setV2DecodedInspection(record, inspection);
      inspection.evidence =
          jf::get32(bytes + kV2RetireOffset) == kErasedWord
              ? PageEvidence::kV2Committed
              : PageEvidence::kV2CommittedRetired;
      return true;
    }

    if (body_valid) {
      setV2DecodedInspection(record, inspection);
      inspection.evidence = PageEvidence::kV2PartialCommit;
      return true;
    }

    inspection.evidence = PageEvidence::kSupportedCorrupt;
    return true;
  }

  // A partially programmed future body can look like a numeric future
  // version while the untouched suffix is still erased. Torn-prefix evidence
  // wins before the future-schema discriminator.
  if (programmedPrefixWithErasedTail(bytes, kV2PagePrefixSize)) {
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

}  // namespace orun_tlp::config_format
