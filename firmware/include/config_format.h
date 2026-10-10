#pragma once
#include <stddef.h>
#include <stdint.h>

// M7P5: on-flash durable config record format. A sibling of
// journal_format.h (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md sec 12
// requires the config partition to reuse history's proven power-cut
// invariant family, not its storage ownership/code/pages): explicit magic,
// schema version, generation, encoded length, payload, CRC, and a final
// 4-byte commit word programmed separately last. Reuses journal_format's
// already-tested byte-order/CRC32/erased-check primitives directly instead
// of duplicating them.
//
// ConfigStore v2 architecture later fixed an exact tokenized record and a
// page-evidence classifier. PR #46 switches normal ConfigStore runtime writes
// to v2. The legacy v1 constants/functions below remain byte-for-byte frozen
// for compatibility fixtures and read-only legacy-development detection; they
// are not a normal runtime write format after the cutover.
namespace orun_tlp::config_format {

constexpr uint32_t kMagic = 0x4F524331;  // "ORC1"

// ---- Legacy v1 format: production contract remains unchanged. ----
constexpr uint8_t kVersion = 1;
constexpr uint32_t kCommit = 0;
// v1 payload: tracking_interval_seconds (4B) + battery_capacity_mah (4B).
constexpr uint16_t kPayloadSize = 8;
// magic(4) + version+reserved(4) + generation(8) + length+reserved(4) +
// payload(8) + crc32(4) + commit(4).
constexpr uint32_t kRecordSize = 36;

// ---- Tokenized v2 sealed-record contract. ----
// The sealed record is 48 bytes. A page-local token-retire word lives
// immediately after it at page offset 48 and is deliberately outside the
// sealed record CRC.
constexpr uint8_t kV2Version = 2;
constexpr uint16_t kV2PayloadSize = 20;
constexpr uint32_t kV2RecordSize = 48;
constexpr uint32_t kV2CrcOffset = 40;
constexpr uint32_t kV2CommitOffset = 44;
constexpr uint32_t kV2BodyAndCrcSize = 44;
constexpr uint32_t kV2RetireOffset = 48;
constexpr uint32_t kV2PagePrefixSize = 52;
constexpr uint32_t kErasedWord = UINT32_MAX;

static_assert(kV2CrcOffset % 4 == 0, "v2 CRC must be word aligned");
static_assert(kV2CommitOffset % 4 == 0, "v2 commit must be word aligned");
static_assert(kV2RetireOffset % 4 == 0, "v2 retire word must be word aligned");
static_assert(kV2BodyAndCrcSize <= 64, "v2 body+CRC must fit ConfigPort staging");
static_assert(kV2RecordSize == kV2CommitOffset + 4, "v2 record packing");
static_assert(kV2PagePrefixSize == kV2RetireOffset + 4, "v2 page-prefix packing");

// ---- Tokenized v4 sealed-record contract (persisted service intent). ----
// docs/architecture/ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md. Same family and
// write discipline as v2, 68-byte sealed record, page-local retire word at 68.
// v4 firmware reads v2 and v4 and writes only v4.
constexpr uint8_t kV4Version = 4;
constexpr uint16_t kV4PayloadSize = 40;
constexpr uint32_t kV4RecordSize = 68;
constexpr uint32_t kV4ReservedOffset = 44;
constexpr uint32_t kV4ReservedSize = 16;
constexpr uint32_t kV4CrcOffset = 60;
constexpr uint32_t kV4CommitOffset = 64;
constexpr uint32_t kV4BodyAndCrcSize = 64;
constexpr uint32_t kV4RetireOffset = 68;
constexpr uint32_t kV4PagePrefixSize = 72;

static_assert(kV4CrcOffset % 4 == 0, "v4 CRC must be word aligned");
static_assert(kV4CommitOffset % 4 == 0, "v4 commit must be word aligned");
static_assert(kV4RetireOffset % 4 == 0, "v4 retire word must be word aligned");
static_assert(kV4BodyAndCrcSize <= 64, "v4 body+CRC must fit ConfigPort staging");
static_assert(kV4RecordSize == kV4CommitOffset + 4, "v4 record packing");
static_assert(kV4PagePrefixSize == kV4RetireOffset + 4, "v4 page-prefix packing");
static_assert(kV4ReservedOffset + kV4ReservedSize == kV4CrcOffset,
              "v4 reserved room ends at the CRC");
// The v2-only forward-compatibility discriminator requires bytes [48..51] of
// every valid newer record to be non-FF. In v4 they are reserved zeros.
static_assert(kV4ReservedOffset <= 48 && 52 <= kV4CrcOffset,
              "v4 keeps bytes 48..51 inside the zero reserved room");

// The schema every save and every fresh baseline writes.
constexpr uint32_t kWriteRecordSize = kV4RecordSize;
constexpr uint32_t kWriteBodyAndCrcSize = kV4BodyAndCrcSize;
constexpr uint32_t kWriteCommitOffset = kV4CommitOffset;
// Largest owned page prefix of any schema this firmware reads.
constexpr uint32_t kMaxPagePrefixSize = kV4PagePrefixSize;

// Requested-service intent. Requested, not effective: capability and runtime
// state are never stored here.
constexpr uint8_t kServiceModeAuto = 0;      // no explicit selection saved
constexpr uint8_t kServiceModeExplicit = 1;  // requested_services is the intent
constexpr uint8_t kServiceTracking = 0x01;
constexpr uint8_t kServiceRelayForwarding = 0x02;
constexpr uint8_t kServiceApplicationReceive = 0x04;
constexpr uint8_t kKnownServicesMask =
    kServiceTracking | kServiceRelayForwarding | kServiceApplicationReceive;

struct Config {
  // Explicit constructor (not default member initializers): the vendored
  // RAK toolchain builds this firmware under gnu++11, where a struct with
  // default member initializers is not an aggregate and brace-init like
  // Config{a, b} would not compile -- matches the same pattern already used
  // by RequestedConfig/CapabilityState in runtime_config.h.
  constexpr Config(uint32_t tracking_interval_seconds_value = 0,
                   uint32_t battery_capacity_mah_value = 0,
                   uint8_t service_mode_value = kServiceModeAuto,
                   uint8_t requested_services_value = 0)
      : tracking_interval_seconds(tracking_interval_seconds_value),
        battery_capacity_mah(battery_capacity_mah_value),
        service_mode(service_mode_value),
        requested_services(requested_services_value) {}

  uint32_t tracking_interval_seconds;
  uint32_t battery_capacity_mah;
  // v4 only. A decoded v2 record reads as AUTO with no requested services.
  uint8_t service_mode;
  uint8_t requested_services;
};

struct StateToken {
  constexpr StateToken(uint64_t incarnation_value = 0,
                       uint32_t revision_value = 0)
      : incarnation(incarnation_value), revision(revision_value) {}

  uint64_t incarnation;
  uint32_t revision;
};

// One tokenized record, independent of the schema (v2 or v4) it is stored in.
struct V2Record {
  constexpr V2Record(uint64_t generation_value = 0,
                     Config config_value = Config(),
                     StateToken token_value = StateToken())
      : generation(generation_value), config(config_value), token(token_value) {}

  uint64_t generation;
  Config config;
  StateToken token;
};
using TokenizedRecord = V2Record;

// Low-level physical/format evidence only. "Committed" here means the known
// format/CRC/commit structure is coherent; ConfigStore must still apply the
// semantic candidate policy (for example the tracking-interval bounds) before
// mapping structural evidence to an application-authoritative config/token.
//
// Legacy-v1 evidence exists only so the v2 cutover can diagnose an old
// development partition and require an explicit maintenance reset. It is not
// an authorization to implement automatic v1 -> v2 migration.
enum class PageEvidence : uint8_t {
  kErased = 0,
  kLegacyV1Committed,
  kLegacyV1UncommittedOrTorn,
  kLegacyV1CommittedCorrupt,
  kV2Staged,
  kV2UncommittedOrTorn,
  kV2PartialCommit,
  kV2Committed,
  kV2CommittedRetired,
  kV2CommittedCorrupt,
  kSupportedCorrupt,
  kUnsupportedNewer,
  // v4 mirrors the v2 cases one for one. Appended so existing values and
  // their diagnostic names do not move.
  kV4Staged,
  kV4UncommittedOrTorn,
  kV4PartialCommit,
  kV4Committed,
  kV4CommittedRetired,
  kV4CommittedCorrupt,
};

// Schema-neutral views over the tokenized v2/v4 evidence.
bool evidenceIsCommitted(PageEvidence evidence);
bool evidenceIsStaged(PageEvidence evidence);
bool evidenceIsUncommittedOrTorn(PageEvidence evidence);
bool evidenceIsPartialCommit(PageEvidence evidence);
bool evidenceIsCommittedRetired(PageEvidence evidence);
// Bytes of the page this evidence owns; everything after must be erased.
uint32_t ownedPrefixSize(PageEvidence evidence);

struct PageInspection {
  PageInspection()
      : evidence(PageEvidence::kSupportedCorrupt),
        has_decoded_record(false),
        generation(0),
        config(),
        token() {}

  PageEvidence evidence;
  // Structural decode only. This never means semantic validity, page
  // authority, or token VALIDity; the ConfigStore recovery owner decides those.
  bool has_decoded_record;
  uint64_t generation;
  Config config;
  StateToken token;
};

// ---- Legacy v1 codec. ----
// bytes must point at kRecordSize writable/readable bytes.
void encode(const Config& config, uint64_t generation, uint8_t* bytes);

// Fails closed (returns false, leaves generation/config untouched) on a bad
// magic, an unrecognized/newer schema version, a payload-length mismatch, a
// zero generation, nonzero reserved bytes, or a CRC/commit mismatch (torn
// or never-written record). Never guesses at an unknown schema's layout.
bool decode(const uint8_t* bytes, uint64_t& generation, Config& config);

// ---- Tokenized v2 codec/classifier. ----
// encodeV2() emits the complete 48-byte sealed record including commit=0.
// The later v2 ConfigStore runtime cutover will program only bytes [0..43]
// first and program bytes [44..47] separately after activation preconditions.
// Current product scope uses a clean development partition reset, not automatic
// v1 -> v2 semantic migration.
void encodeV2(const V2Record& record, uint8_t* bytes);

// decodeV2Body() validates bytes [0..43] independently of commit state.
// decodeV2() additionally requires commit==0. Neither function evaluates
// product-level Config semantic bounds; that remains ConfigStore policy.
bool decodeV2Body(const uint8_t* bytes, V2Record& record);
bool decodeV2(const uint8_t* bytes, V2Record& record);

// ---- Tokenized v4 codec. ----
// encodeV4() emits the complete 68-byte sealed record including commit=0;
// ConfigStore programs bytes [0..63] first and [64..67] separately last.
// decodeV4Body() validates bytes [0..63] (structure, reserved zeros, CRC,
// token) independently of commit state; decodeV4() also requires commit==0.
// Neither evaluates service-mode/service-bit semantics: an unknown mode or
// bit decodes, and ConfigStore treats it as semantically invalid.
void encodeV4(const TokenizedRecord& record, uint8_t* bytes);
bool decodeV4Body(const uint8_t* bytes, TokenizedRecord& record);
bool decodeV4(const uint8_t* bytes, TokenizedRecord& record);

// Classifies the owned prefix of one ConfigStore page. At least
// kV2PagePrefixSize bytes are required; a v4 page needs kV4PagePrefixSize
// bytes (with fewer, a v4 page is reported as supported corruption).
// The ordering implements the reviewed forward-compatibility contract:
// erased -> exact v1/v2 -> recognizable torn prefix -> unsupported-newer
// discriminator -> supported local corruption.
//
// Any future deployable ORC1 schema that relies on this classifier contract must
// keep bytes[5..7]==0, use the reserved multiple-of-four version namespace, and
// ensure bytes[48..51] are non-FF in every valid non-torn record. Otherwise an
// older v2 classifier may conservatively treat it as local torn/corrupt evidence
// instead of UNSUPPORTED_NEWER. Version 0 is reserved/incompatible, not a
// deployable future schema version.
//
// Returns false only for invalid arguments (nullptr or too-short input).
// On true, inspection.evidence always contains a classification.
bool inspectPagePrefix(const uint8_t* bytes, size_t size,
                       PageInspection& inspection);

}  // namespace orun_tlp::config_format
