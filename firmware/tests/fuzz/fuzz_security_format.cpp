#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "journal_format.h"
#include "security_format.h"

namespace jf = orun_tlp::journal_format;
namespace sf = orun_tlp::security_format;

namespace {
constexpr size_t kSerializedWordSize = sizeof(uint32_t);

constexpr size_t kPageHeaderCrcOffset =
    sizeof(uint32_t) + sizeof(uint8_t) + 3U + sizeof(uint64_t) +
    sizeof(uint64_t);
constexpr size_t kPageHeaderCommitOffset =
    kPageHeaderCrcOffset + kSerializedWordSize;
static_assert(kPageHeaderCrcOffset == 24, "security page-header CRC layout");
static_assert(kPageHeaderCommitOffset + kSerializedWordSize ==
                  sf::kPageHeaderSize,
              "security page-header record layout");

constexpr size_t kCredentialCrcOffset =
    sf::kCredentialIdSize + sizeof(uint32_t) + sizeof(uint64_t) +
    sf::kKRootSize;
constexpr size_t kCredentialCommitOffset =
    kCredentialCrcOffset + kSerializedWordSize;
static_assert(kCredentialCrcOffset == 60, "security credential CRC layout");
static_assert(kCredentialCommitOffset + kSerializedWordSize ==
                  sf::kCredentialRecordSize,
              "security credential record layout");

constexpr size_t kTxReserveCrcOffset =
    sf::kCredentialIdSize + sizeof(uint32_t) + sizeof(uint64_t);
constexpr size_t kTxReserveCommitOffset =
    kTxReserveCrcOffset + kSerializedWordSize;
static_assert(kTxReserveCrcOffset == 28, "security TX reserve CRC layout");
static_assert(kTxReserveCommitOffset + kSerializedWordSize ==
                  sf::kTxReserveRecordSize,
              "security TX reserve record layout");

constexpr size_t kSecurityStateCrcOffset =
    sf::kCredentialIdSize + sizeof(uint32_t) + sizeof(uint8_t) + 3U +
    sizeof(uint64_t);
constexpr size_t kSecurityStateCommitOffset =
    kSecurityStateCrcOffset + kSerializedWordSize;
static_assert(kSecurityStateCrcOffset == 32, "security state CRC layout");
static_assert(kSecurityStateCommitOffset + kSerializedWordSize ==
                  sf::kSecurityStateRecordSize,
              "security state record layout");

uint64_t fold64(const uint8_t* data, size_t size, size_t start) {
  uint64_t value = 0;
  if (size == 0) return 0;
  for (size_t i = 0; i < 8; ++i)
    value = (value << 8) | data[(start + i) % size];
  return value;
}

uint32_t fold32(const uint8_t* data, size_t size, size_t start) {
  uint32_t value = 0;
  if (size == 0) return 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | data[(start + i) % size];
  return value;
}

uint8_t deltaByte(const uint8_t* data, size_t size, size_t start) {
  if (size == 0) return 1;
  const uint8_t value = data[start % size];
  return value == 0 ? 1U : value;
}

void mutateAndReseal(uint8_t* bytes, size_t body_size, size_t crc_offset,
                     size_t commit_offset, const uint8_t* data, size_t size,
                     size_t selector_start, size_t delta_start) {
  if (size > 1) {
    const size_t index = fold32(data, size, selector_start) % body_size;
    bytes[index] ^= deltaByte(data, size, delta_start);
  }
  jf::put32(bytes + crc_offset, jf::crc32(bytes, crc_offset));
  jf::put32(bytes + commit_offset, sf::kCommit);
}

void checkHeaderRoundTrip(const uint8_t* bytes, uint8_t version) {
  sf::PageHeader decoded;
  if (!sf::decodePageHeaderVersion(bytes, version, decoded)) return;
  uint8_t encoded[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(decoded, version, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}

void checkCredentialRoundTrip(const uint8_t* bytes) {
  sf::Credential decoded;
  if (!sf::decodeCredential(bytes, decoded)) return;
  uint8_t encoded[sf::kCredentialRecordSize]{};
  sf::encodeCredential(decoded, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}

void checkTxRoundTrip(const uint8_t* bytes) {
  sf::TxReserve decoded;
  if (!sf::decodeTxReserve(bytes, decoded)) return;
  uint8_t encoded[sf::kTxReserveRecordSize]{};
  sf::encodeTxReserve(decoded, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}

void checkStateRoundTrip(const uint8_t* bytes) {
  sf::SecurityStateRecord decoded;
  if (!sf::decodeSecurityState(bytes, decoded)) return;
  uint8_t encoded[sf::kSecurityStateRecordSize]{};
  sf::encodeSecurityState(decoded, encoded);
  if (memcmp(encoded, bytes, sizeof(encoded)) != 0) __builtin_trap();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Raw malformed-input paths.
  if (size >= sf::kPageHeaderSize) {
    checkHeaderRoundTrip(data, sf::kVersionV1);
    checkHeaderRoundTrip(data, sf::kVersionV2);
    uint8_t version = 0;
    (void)sf::headerMagicPresent(data, &version);
  }
  if (size >= sf::kCredentialRecordSize) checkCredentialRoundTrip(data);
  if (size >= sf::kTxReserveRecordSize) checkTxRoundTrip(data);
  if (size >= sf::kSecurityStateRecordSize) checkStateRoundTrip(data);
  if (size == 0) return 0;

  // Page header: exercise both supported v1 and v2 decode paths, then preserve
  // CRC/commit after a body mutation so semantic validation is reached.
  const sf::PageHeader header(fold64(data, size, 0) | 1ULL,
                              fold64(data, size, 8));
  const uint8_t versions[2] = {sf::kVersionV1, sf::kVersionV2};
  for (const uint8_t version : versions) {
    uint8_t encoded[sf::kPageHeaderSize]{};
    sf::encodePageHeaderVersion(header, version, encoded);
    checkHeaderRoundTrip(encoded, version);
    mutateAndReseal(encoded, kPageHeaderCrcOffset, kPageHeaderCrcOffset,
                    kPageHeaderCommitOffset, data, size, 16, 20);
    checkHeaderRoundTrip(encoded, version);
  }

  sf::Credential credential;
  for (size_t i = 0; i < sf::kCredentialIdSize; ++i)
    credential.credential_id[i] = data[i % size];
  credential.key_epoch = fold32(data, size, 16);
  credential.device_identity = fold64(data, size, 20);
  for (size_t i = 0; i < sf::kKRootSize; ++i)
    credential.k_root[i] = data[(28 + i) % size];

  uint8_t credential_bytes[sf::kCredentialRecordSize]{};
  sf::encodeCredential(credential, credential_bytes);
  checkCredentialRoundTrip(credential_bytes);
  mutateAndReseal(credential_bytes, kCredentialCrcOffset,
                  kCredentialCrcOffset, kCredentialCommitOffset, data, size,
                  36, 40);
  checkCredentialRoundTrip(credential_bytes);

  sf::TxReserve reserve;
  for (size_t i = 0; i < sf::kCredentialIdSize; ++i)
    reserve.credential_id[i] = data[(8 + i) % size];
  reserve.key_epoch = fold32(data, size, 24);
  reserve.tx_reserved_bound =
      fold64(data, size, 28) & ~(sf::kTxReservationBlockSize - 1ULL);
  if (reserve.tx_reserved_bound == 0)
    reserve.tx_reserved_bound = sf::kTxReservationBlockSize;

  uint8_t reserve_bytes[sf::kTxReserveRecordSize]{};
  sf::encodeTxReserve(reserve, reserve_bytes);
  checkTxRoundTrip(reserve_bytes);
  mutateAndReseal(reserve_bytes, kTxReserveCrcOffset, kTxReserveCrcOffset,
                  kTxReserveCommitOffset, data, size, 44, 48);
  checkTxRoundTrip(reserve_bytes);

  sf::SecurityStateRecord state;
  for (size_t i = 0; i < sf::kCredentialIdSize; ++i)
    state.credential_id[i] = data[(16 + i) % size];
  state.key_epoch = fold32(data, size, 32);
  state.kind = (data[0] & 1U)
                   ? sf::SecurityStateKind::kTxReserveExclusiveBound
                   : sf::SecurityStateKind::kA2dReplayExclusiveBound;
  const uint64_t block =
      state.kind == sf::SecurityStateKind::kTxReserveExclusiveBound
          ? sf::kTxReservationBlockSize
          : sf::kA2dReplayReservationBlockSize;
  state.value = fold64(data, size, 36) & ~(block - 1ULL);
  if (state.value == 0) state.value = block;

  uint8_t state_bytes[sf::kSecurityStateRecordSize]{};
  sf::encodeSecurityState(state, state_bytes);
  checkStateRoundTrip(state_bytes);
  mutateAndReseal(state_bytes, kSecurityStateCrcOffset,
                  kSecurityStateCrcOffset, kSecurityStateCommitOffset, data,
                  size, 52, 56);
  checkStateRoundTrip(state_bytes);

  return 0;
}
