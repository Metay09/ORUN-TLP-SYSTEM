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

bool validV4Body(const uint8_t* bytes, TokenizedRecord& record) {
  if (jf::get32(bytes) != kMagic) return false;
  if (bytes[4] != kV4Version || bytes[5] || bytes[6] || bytes[7])
    return false;

  const uint64_t generation = jf::get64(bytes + 8);
  if (generation == 0) return false;
  if (jf::get16(bytes + 16) != kV4PayloadSize ||
      jf::get16(bytes + 18) != 0)
    return false;
  if (bytes[30] != 0 || bytes[31] != 0) return false;
  for (uint32_t i = 0; i < kV4ReservedSize; ++i)
    if (bytes[kV4ReservedOffset + i] != 0) return false;

  if (jf::get32(bytes + kV4CrcOffset) != jf::crc32(bytes, kV4CrcOffset))
    return false;

  const uint64_t incarnation = jf::get64(bytes + 32);
  const uint32_t revision = jf::get32(bytes + 40);
  if (incarnation == 0 || revision == 0) return false;

  TokenizedRecord decoded(
      generation,
      Config(jf::get32(bytes + 20), jf::get32(bytes + 24), bytes[28],
             bytes[29]),
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
  if (version == kVersion || version == kV2Version || version == kV4Version ||
      version == 0xFF)
    return false;
  return bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
         (version & 0x03U) == 0;
}

void setV1Inspection(const uint8_t* bytes, PageInspection& inspection) {
  uint64_t generation = 0;
  Config config;
  if (decode(bytes, generation, config)) {
    inspection.evidence = PageEvidence::kLegacyV1Committed;
    inspection.has_decoded_record = true;
    inspection.generation = generation;
    inspection.config = config;
    return;
  }
  inspection.evidence = PageEvidence::kLegacyV1CommittedCorrupt;
}

void setV2DecodedInspection(const V2Record& record,
                            PageInspection& inspection) {
  inspection.has_decoded_record = true;
  inspection.generation = record.generation;
  inspection.config = record.config;
  inspection.token = record.token;
}

}  // namespace

bool evidenceIsCommitted(PageEvidence evidence) {
  return evidence == PageEvidence::kV2Committed ||
         evidence == PageEvidence::kV4Committed;
}

bool evidenceIsStaged(PageEvidence evidence) {
  return evidence == PageEvidence::kV2Staged ||
         evidence == PageEvidence::kV4Staged;
}

bool evidenceIsUncommittedOrTorn(PageEvidence evidence) {
  return evidence == PageEvidence::kV2UncommittedOrTorn ||
         evidence == PageEvidence::kV4UncommittedOrTorn;
}

bool evidenceIsPartialCommit(PageEvidence evidence) {
  return evidence == PageEvidence::kV2PartialCommit ||
         evidence == PageEvidence::kV4PartialCommit;
}

bool evidenceIsCommittedRetired(PageEvidence evidence) {
  return evidence == PageEvidence::kV2CommittedRetired ||
         evidence == PageEvidence::kV4CommittedRetired;
}

uint32_t ownedPrefixSize(PageEvidence evidence) {
  switch (evidence) {
    case PageEvidence::kV4Staged:
    case PageEvidence::kV4UncommittedOrTorn:
    case PageEvidence::kV4PartialCommit:
    case PageEvidence::kV4Committed:
    case PageEvidence::kV4CommittedRetired:
    case PageEvidence::kV4CommittedCorrupt:
      return kV4PagePrefixSize;
    default:
      return kV2PagePrefixSize;
  }
}

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

void encodeV4(const TokenizedRecord& record, uint8_t* bytes) {
  memset(bytes, 0, kV4RecordSize);
  jf::put32(bytes, kMagic);
  bytes[4] = kV4Version;
  jf::put64(bytes + 8, record.generation);
  jf::put16(bytes + 16, kV4PayloadSize);
  jf::put32(bytes + 20, record.config.tracking_interval_seconds);
  jf::put32(bytes + 24, record.config.battery_capacity_mah);
  bytes[28] = record.config.service_mode;
  bytes[29] = record.config.requested_services;
  jf::put64(bytes + 32, record.token.incarnation);
  jf::put32(bytes + 40, record.token.revision);
  jf::put32(bytes + kV4CrcOffset, jf::crc32(bytes, kV4CrcOffset));
  jf::put32(bytes + kV4CommitOffset, kCommit);
}

bool decodeV4Body(const uint8_t* bytes, TokenizedRecord& record) {
  if (bytes == nullptr) return false;
  TokenizedRecord decoded;
  if (!validV4Body(bytes, decoded)) return false;
  record = decoded;
  return true;
}

bool decodeV4(const uint8_t* bytes, TokenizedRecord& record) {
  if (bytes == nullptr || jf::get32(bytes + kV4CommitOffset) != kCommit)
    return false;
  return decodeV4Body(bytes, record);
}

bool inspectPagePrefix(const uint8_t* bytes, size_t size,
                       PageInspection& inspection) {
  if (bytes == nullptr || size < kV2PagePrefixSize) return false;

  inspection = PageInspection();

  // Everything the caller supplied up to the largest owned prefix must be
  // erased; the page tail after that is checked by ConfigStore.
  const size_t owned =
      size < kMaxPagePrefixSize ? size : static_cast<size_t>(kMaxPagePrefixSize);
  if (jf::erased(bytes, owned)) {
    inspection.evidence = PageEvidence::kErased;
    return true;
  }

  const bool magic_matches = jf::get32(bytes) == kMagic;
  const uint8_t version = bytes[4];

  // Exact supported v1 is inspected before any generic torn/future rule.
  if (magic_matches && version == kVersion) {
    const uint32_t commit = jf::get32(bytes + kV1CommitOffset);
    if (commit == kErasedWord) {
      inspection.evidence = PageEvidence::kLegacyV1UncommittedOrTorn;
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

  // Exact supported v4: same evidence shapes as v2 at the v4 offsets.
  if (magic_matches && version == kV4Version) {
    if (size < kV4PagePrefixSize) {
      inspection.evidence = PageEvidence::kSupportedCorrupt;
      return true;
    }
    TokenizedRecord record;
    const bool body_valid = validV4Body(bytes, record);
    const uint32_t commit = jf::get32(bytes + kV4CommitOffset);

    if (commit == kErasedWord) {
      inspection.evidence = body_valid ? PageEvidence::kV4Staged
                                       : PageEvidence::kV4UncommittedOrTorn;
      if (body_valid) setV2DecodedInspection(record, inspection);
      return true;
    }

    if (commit == kCommit) {
      if (!body_valid) {
        inspection.evidence = PageEvidence::kV4CommittedCorrupt;
        return true;
      }
      setV2DecodedInspection(record, inspection);
      inspection.evidence =
          jf::get32(bytes + kV4RetireOffset) == kErasedWord
              ? PageEvidence::kV4Committed
              : PageEvidence::kV4CommittedRetired;
      return true;
    }

    if (body_valid) {
      setV2DecodedInspection(record, inspection);
      inspection.evidence = PageEvidence::kV4PartialCommit;
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
