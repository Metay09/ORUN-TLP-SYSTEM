#pragma once
#include <stddef.h>
#include <stdint.h>

// Durable SecurityStore record formats.
//
// v1 (M7P6B) remains a read/migration compatibility format:
//   32 B page header + 68 B credential + 111 * 36 B TX_RESERVE = 4096 B.
//
// v2 (M7P6F) keeps the header/credential shapes but replaces the TX-only tail
// with a 40-byte typed SECURITY_STATE append log. The only authorized state
// kinds are TX reserve exclusive bound and A2D replay exclusive bound. No
// generic state/plugin registry is introduced.
//
// Page activation remains commit-last: the final page-header word is programmed
// only after the complete new-page snapshot is durable/read-verified.
namespace orun_tlp::security_format {

constexpr uint32_t kMagic = 0x4F525331;  // "ORS1"
constexpr uint8_t kVersionV1 = 1;
constexpr uint8_t kVersionV2 = 2;
constexpr uint8_t kVersion = kVersionV2;  // current write format
constexpr uint32_t kCommit = 0;

constexpr size_t kCredentialIdSize = 16;
constexpr size_t kKRootSize = 32;

constexpr uint32_t kPageHeaderSize = 32;
constexpr uint32_t kCredentialRecordSize = 68;

// Legacy v1 TX_RESERVE record:
// credential_id(16) + key_epoch(4) + tx_reserved_bound(8) + crc32(4) +
// commit(4).
constexpr uint32_t kTxReserveRecordSize = 36;
constexpr uint32_t kV1TxReserveSlotsPerPage =
    (4096 - kPageHeaderSize - kCredentialRecordSize) / kTxReserveRecordSize;
// Compatibility alias for v1-only tests/helpers. New v2 code must use the
// SECURITY_STATE constants below.
constexpr uint32_t kTxReserveSlotsPerPage = kV1TxReserveSlotsPerPage;
static_assert(kPageHeaderSize + kCredentialRecordSize +
                      kV1TxReserveSlotsPerPage * kTxReserveRecordSize ==
                  4096,
              "security v1 page must pack exactly into one 4096-byte page");

// v2 SECURITY_STATE:
// credential_id(16) + key_epoch(4) + kind(1) + reserved(3) + value(8) +
// crc32(4) + commit(4).
constexpr uint32_t kSecurityStateRecordSize = 40;
constexpr uint32_t kSecurityStateSlotsPerPage =
    (4096 - kPageHeaderSize - kCredentialRecordSize) / kSecurityStateRecordSize;
constexpr uint32_t kSecurityStateTailBytes =
    4096 - kPageHeaderSize - kCredentialRecordSize -
    kSecurityStateSlotsPerPage * kSecurityStateRecordSize;
static_assert(kSecurityStateSlotsPerPage == 99,
              "security v2 must provide exactly 99 state slots");
static_assert(kSecurityStateTailBytes == 36,
              "security v2 must leave the reviewed 36-byte erased tail");

constexpr uint64_t kTxReservationBlockSize = 256;
constexpr uint64_t kA2dReplayReservationBlockSize = 8;

uint32_t pageHeaderOffset();
uint32_t credentialRecordOffset();
uint32_t txReserveRecordOffset(unsigned slot);       // v1 only
uint32_t securityStateRecordOffset(unsigned slot);   // v2 only

struct PageHeader {
  constexpr PageHeader(uint64_t generation_value = 0,
                       uint64_t device_identity_value = 0)
      : generation(generation_value), device_identity(device_identity_value) {}
  uint64_t generation;
  uint64_t device_identity;
};

// Current-format (v2) encoder/decoder used for all new writes.
void encodePageHeader(const PageHeader& header, uint8_t* bytes);
bool decodePageHeader(const uint8_t* bytes, PageHeader& header);

// Explicit version helpers are used only for reviewed v1 recovery/migration and
// malformed/golden tests. Supported values are kVersionV1 and kVersionV2.
void encodePageHeaderVersion(const PageHeader& header, uint8_t version,
                             uint8_t* bytes);
bool decodePageHeaderVersion(const uint8_t* bytes, uint8_t expected_version,
                             PageHeader& header);

// Returns true and exposes the version whenever the ORS1 magic is present,
// including an unsupported future version. Blank/wrong-magic bytes return false.
bool headerMagicPresent(const uint8_t* bytes, uint8_t* version);

struct Credential {
  uint8_t credential_id[kCredentialIdSize]{};
  uint32_t key_epoch = 0;
  uint64_t device_identity = 0;
  uint8_t k_root[kKRootSize]{};
};

void encodeCredential(const Credential& credential, uint8_t* bytes);
bool decodeCredential(const uint8_t* bytes, Credential& credential);
bool credentialIdEqual(const uint8_t (&a)[kCredentialIdSize],
                       const uint8_t (&b)[kCredentialIdSize]);

// Legacy v1-only record retained so new firmware can recover/migrate an
// authoritative v1 page without reinterpreting any old byte.
struct TxReserve {
  uint8_t credential_id[kCredentialIdSize]{};
  uint32_t key_epoch = 0;
  uint64_t tx_reserved_bound = 0;
};
void encodeTxReserve(const TxReserve& reserve, uint8_t* bytes);
bool decodeTxReserve(const uint8_t* bytes, TxReserve& reserve);

enum class SecurityStateKind : uint8_t {
  kTxReserveExclusiveBound = 1,
  kA2dReplayExclusiveBound = 2,
};

struct SecurityStateRecord {
  uint8_t credential_id[kCredentialIdSize]{};
  uint32_t key_epoch = 0;
  SecurityStateKind kind = SecurityStateKind::kTxReserveExclusiveBound;
  uint64_t value = 0;
};

// v2 only. decode fails closed on CRC/commit failure, nonzero reserved bytes,
// unknown kind, zero value, or a value not aligned to that kind's reviewed
// reservation block.
void encodeSecurityState(const SecurityStateRecord& state, uint8_t* bytes);
bool decodeSecurityState(const uint8_t* bytes, SecurityStateRecord& state);

}  // namespace orun_tlp::security_format
