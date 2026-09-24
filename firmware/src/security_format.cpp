#include "security_format.h"

#include <string.h>

#include "journal_format.h"

namespace orun_tlp::security_format {
namespace jf = orun_tlp::journal_format;
namespace {
constexpr unsigned kHeaderVersionOffset = 4;
constexpr unsigned kHeaderGenerationOffset = 8;
constexpr unsigned kHeaderDeviceIdentityOffset = 16;
constexpr unsigned kHeaderCrcOffset = 24;
constexpr unsigned kHeaderCommitOffset = 28;

constexpr unsigned kCredIdOffset = 0;
constexpr unsigned kCredKeyEpochOffset = kCredIdOffset + kCredentialIdSize;
constexpr unsigned kCredDeviceIdentityOffset = kCredKeyEpochOffset + 4;
constexpr unsigned kCredKRootOffset = kCredDeviceIdentityOffset + 8;
constexpr unsigned kCredCrcOffset = kCredKRootOffset + kKRootSize;
constexpr unsigned kCredCommitOffset = kCredCrcOffset + 4;
static_assert(kCredCommitOffset + 4 == kCredentialRecordSize,
              "credential packing");

constexpr unsigned kTxIdOffset = 0;
constexpr unsigned kTxKeyEpochOffset = kTxIdOffset + kCredentialIdSize;
constexpr unsigned kTxBoundOffset = kTxKeyEpochOffset + 4;
constexpr unsigned kTxCrcOffset = kTxBoundOffset + 8;
constexpr unsigned kTxCommitOffset = kTxCrcOffset + 4;
static_assert(kTxCommitOffset + 4 == kTxReserveRecordSize,
              "tx_reserve packing");

constexpr unsigned kStateIdOffset = 0;
constexpr unsigned kStateKeyEpochOffset = kStateIdOffset + kCredentialIdSize;
constexpr unsigned kStateKindOffset = kStateKeyEpochOffset + 4;
constexpr unsigned kStateReservedOffset = kStateKindOffset + 1;
constexpr unsigned kStateValueOffset = kStateReservedOffset + 3;
constexpr unsigned kStateCrcOffset = kStateValueOffset + 8;
constexpr unsigned kStateCommitOffset = kStateCrcOffset + 4;
static_assert(kStateCommitOffset + 4 == kSecurityStateRecordSize,
              "security_state packing");

void seal(uint8_t* bytes, unsigned crc_offset, unsigned commit_offset) {
  jf::put32(bytes + crc_offset, jf::crc32(bytes, crc_offset));
  jf::put32(bytes + commit_offset, kCommit);
}

bool sealed(const uint8_t* bytes, unsigned crc_offset,
            unsigned commit_offset) {
  return jf::get32(bytes + commit_offset) == kCommit &&
         jf::get32(bytes + crc_offset) == jf::crc32(bytes, crc_offset);
}

bool supportedVersion(uint8_t version) {
  return version == kVersionV1 || version == kVersionV2;
}

bool validStateValue(SecurityStateKind kind, uint64_t value) {
  if (value == 0) return false;
  switch (kind) {
    case SecurityStateKind::kTxReserveExclusiveBound:
      return value % kTxReservationBlockSize == 0;
    case SecurityStateKind::kA2dReplayExclusiveBound:
      return value % kA2dReplayReservationBlockSize == 0;
  }
  return false;
}
}  // namespace

uint32_t pageHeaderOffset() { return 0; }

uint32_t credentialRecordOffset() { return kPageHeaderSize; }

uint32_t txReserveRecordOffset(unsigned slot) {
  return kPageHeaderSize + kCredentialRecordSize +
         slot * kTxReserveRecordSize;
}

uint32_t securityStateRecordOffset(unsigned slot) {
  return kPageHeaderSize + kCredentialRecordSize +
         slot * kSecurityStateRecordSize;
}

void encodePageHeaderVersion(const PageHeader& header, uint8_t version,
                             uint8_t* bytes) {
  memset(bytes, 0, kPageHeaderSize);
  jf::put32(bytes, kMagic);
  bytes[kHeaderVersionOffset] = version;
  jf::put64(bytes + kHeaderGenerationOffset, header.generation);
  jf::put64(bytes + kHeaderDeviceIdentityOffset, header.device_identity);
  seal(bytes, kHeaderCrcOffset, kHeaderCommitOffset);
}

void encodePageHeader(const PageHeader& header, uint8_t* bytes) {
  encodePageHeaderVersion(header, kVersion, bytes);
}

bool headerMagicPresent(const uint8_t* bytes, uint8_t* version) {
  if (jf::erased(bytes, kPageHeaderSize)) return false;
  if (jf::get32(bytes) != kMagic) return false;
  if (version) *version = bytes[kHeaderVersionOffset];
  return true;
}

bool decodePageHeaderVersion(const uint8_t* bytes, uint8_t expected_version,
                             PageHeader& header) {
  if (!supportedVersion(expected_version)) return false;
  if (jf::get32(bytes) != kMagic) return false;
  if (bytes[kHeaderVersionOffset] != expected_version ||
      bytes[kHeaderVersionOffset + 1] ||
      bytes[kHeaderVersionOffset + 2] ||
      bytes[kHeaderVersionOffset + 3])
    return false;
  const uint64_t generation = jf::get64(bytes + kHeaderGenerationOffset);
  if (generation == 0) return false;
  if (!sealed(bytes, kHeaderCrcOffset, kHeaderCommitOffset)) return false;
  header.generation = generation;
  header.device_identity = jf::get64(bytes + kHeaderDeviceIdentityOffset);
  return true;
}

bool decodePageHeader(const uint8_t* bytes, PageHeader& header) {
  return decodePageHeaderVersion(bytes, kVersion, header);
}

void encodeCredential(const Credential& credential, uint8_t* bytes) {
  memset(bytes, 0, kCredentialRecordSize);
  memcpy(bytes + kCredIdOffset, credential.credential_id, kCredentialIdSize);
  jf::put32(bytes + kCredKeyEpochOffset, credential.key_epoch);
  jf::put64(bytes + kCredDeviceIdentityOffset, credential.device_identity);
  memcpy(bytes + kCredKRootOffset, credential.k_root, kKRootSize);
  seal(bytes, kCredCrcOffset, kCredCommitOffset);
}

bool decodeCredential(const uint8_t* bytes, Credential& credential) {
  if (!sealed(bytes, kCredCrcOffset, kCredCommitOffset)) return false;
  memcpy(credential.credential_id, bytes + kCredIdOffset, kCredentialIdSize);
  credential.key_epoch = jf::get32(bytes + kCredKeyEpochOffset);
  credential.device_identity = jf::get64(bytes + kCredDeviceIdentityOffset);
  memcpy(credential.k_root, bytes + kCredKRootOffset, kKRootSize);
  return true;
}

bool credentialIdEqual(const uint8_t (&a)[kCredentialIdSize],
                       const uint8_t (&b)[kCredentialIdSize]) {
  return memcmp(a, b, kCredentialIdSize) == 0;
}

void encodeTxReserve(const TxReserve& reserve, uint8_t* bytes) {
  memset(bytes, 0, kTxReserveRecordSize);
  memcpy(bytes + kTxIdOffset, reserve.credential_id, kCredentialIdSize);
  jf::put32(bytes + kTxKeyEpochOffset, reserve.key_epoch);
  jf::put64(bytes + kTxBoundOffset, reserve.tx_reserved_bound);
  seal(bytes, kTxCrcOffset, kTxCommitOffset);
}

bool decodeTxReserve(const uint8_t* bytes, TxReserve& reserve) {
  if (!sealed(bytes, kTxCrcOffset, kTxCommitOffset)) return false;
  const uint64_t bound = jf::get64(bytes + kTxBoundOffset);
  if (bound == 0 || bound % kTxReservationBlockSize != 0) return false;
  memcpy(reserve.credential_id, bytes + kTxIdOffset, kCredentialIdSize);
  reserve.key_epoch = jf::get32(bytes + kTxKeyEpochOffset);
  reserve.tx_reserved_bound = bound;
  return true;
}

void encodeSecurityState(const SecurityStateRecord& state, uint8_t* bytes) {
  memset(bytes, 0, kSecurityStateRecordSize);
  memcpy(bytes + kStateIdOffset, state.credential_id, kCredentialIdSize);
  jf::put32(bytes + kStateKeyEpochOffset, state.key_epoch);
  bytes[kStateKindOffset] = static_cast<uint8_t>(state.kind);
  jf::put64(bytes + kStateValueOffset, state.value);
  seal(bytes, kStateCrcOffset, kStateCommitOffset);
}

bool decodeSecurityState(const uint8_t* bytes, SecurityStateRecord& state) {
  if (!sealed(bytes, kStateCrcOffset, kStateCommitOffset)) return false;
  if (bytes[kStateReservedOffset] || bytes[kStateReservedOffset + 1] ||
      bytes[kStateReservedOffset + 2])
    return false;

  const uint8_t kind_byte = bytes[kStateKindOffset];
  SecurityStateKind kind;
  if (kind_byte ==
      static_cast<uint8_t>(SecurityStateKind::kTxReserveExclusiveBound)) {
    kind = SecurityStateKind::kTxReserveExclusiveBound;
  } else if (
      kind_byte ==
      static_cast<uint8_t>(SecurityStateKind::kA2dReplayExclusiveBound)) {
    kind = SecurityStateKind::kA2dReplayExclusiveBound;
  } else {
    return false;
  }

  const uint64_t value = jf::get64(bytes + kStateValueOffset);
  if (!validStateValue(kind, value)) return false;

  memcpy(state.credential_id, bytes + kStateIdOffset, kCredentialIdSize);
  state.key_epoch = jf::get32(bytes + kStateKeyEpochOffset);
  state.kind = kind;
  state.value = value;
  return true;
}

}  // namespace orun_tlp::security_format
