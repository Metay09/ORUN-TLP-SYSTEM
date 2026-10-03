#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "security_format.h"

namespace sf = orun_tlp::security_format;

namespace {
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
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size >= sf::kPageHeaderSize) {
    sf::PageHeader header;
    if (sf::decodePageHeader(data, header)) {
      uint8_t encoded[sf::kPageHeaderSize]{};
      sf::encodePageHeader(header, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size >= sf::kCredentialRecordSize) {
    sf::Credential credential;
    if (sf::decodeCredential(data, credential)) {
      uint8_t encoded[sf::kCredentialRecordSize]{};
      sf::encodeCredential(credential, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size >= sf::kTxReserveRecordSize) {
    sf::TxReserve reserve;
    if (sf::decodeTxReserve(data, reserve)) {
      uint8_t encoded[sf::kTxReserveRecordSize]{};
      sf::encodeTxReserve(reserve, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size >= sf::kSecurityStateRecordSize) {
    sf::SecurityStateRecord state;
    if (sf::decodeSecurityState(data, state)) {
      uint8_t encoded[sf::kSecurityStateRecordSize]{};
      sf::encodeSecurityState(state, encoded);
      if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
    }
  }

  if (size == 0) return 0;

  sf::SecurityStateRecord candidate;
  for (size_t i = 0; i < sf::kCredentialIdSize; ++i)
    candidate.credential_id[i] = data[i % size];
  candidate.key_epoch = fold32(data, size, 16);
  candidate.kind = (data[0] & 1U)
                       ? sf::SecurityStateKind::kTxReserveExclusiveBound
                       : sf::SecurityStateKind::kA2dReplayExclusiveBound;
  const uint64_t block =
      candidate.kind == sf::SecurityStateKind::kTxReserveExclusiveBound
          ? sf::kTxReservationBlockSize
          : sf::kA2dReplayReservationBlockSize;
  candidate.value = fold64(data, size, 20) & ~(block - 1U);
  if (candidate.value == 0) candidate.value = block;

  uint8_t encoded[sf::kSecurityStateRecordSize]{};
  sf::encodeSecurityState(candidate, encoded);
  if (size > 1) {
    const size_t index = data[0] % sizeof(encoded);
    const uint8_t delta = data[1] == 0 ? 1U : data[1];
    encoded[index] ^= delta;
  }

  sf::SecurityStateRecord decoded;
  (void)sf::decodeSecurityState(encoded, decoded);
  return 0;
}
