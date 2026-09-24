// M7P6B: security_format encode/decode round-trip and fail-safe corruption
// handling, against the actual production module (pure byte-level checks
// only -- no flash backend involved here).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "security_format.h"

using namespace orun_tlp::security_format;

namespace {
void fillId(uint8_t (&id)[kCredentialIdSize], uint8_t seed) {
  for (size_t i = 0; i < kCredentialIdSize; ++i) id[i] = static_cast<uint8_t>(seed + i);
}
void fillKRoot(uint8_t (&k)[kKRootSize], uint8_t seed) {
  for (size_t i = 0; i < kKRootSize; ++i) k[i] = static_cast<uint8_t>(seed * 3 + i);
}
}  // namespace

int main() {
  // ---- Page header ----
  {
    uint8_t bytes[kPageHeaderSize];
    PageHeader original{42, 0x0E8ADE7E71531AA3ULL};
    encodePageHeader(original, bytes);
    PageHeader decoded{};
    assert(decodePageHeader(bytes, decoded));
    assert(decoded.generation == 42);
    assert(decoded.device_identity == 0x0E8ADE7E71531AA3ULL);
  }
  // Explicit v1 compatibility: new firmware must decode the old page header
  // only through the versioned migration path; the current decoder remains v2.
  {
    uint8_t bytes[kPageHeaderSize];
    PageHeader original{7, 0x0E8ADE7E71531AA3ULL};
    encodePageHeaderVersion(original, kVersionV1, bytes);
    uint8_t version = 0;
    assert(headerMagicPresent(bytes, &version));
    assert(version == kVersionV1);
    PageHeader decoded{};
    assert(decodePageHeaderVersion(bytes, kVersionV1, decoded));
    assert(decoded.generation == 7);
    assert(decoded.device_identity == original.device_identity);
    assert(!decodePageHeader(bytes, decoded));
  }

  // Exact v1/v2 page-header golden bytes. These pin version, byte order,
  // offsets, CRC and activation word; migration compatibility must never rely
  // on native struct layout.
  {
    static const uint8_t kV1Golden[kPageHeaderSize] = {
        0x4F,0x52,0x53,0x31,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,
        0x0E,0x8A,0xDE,0x7E,0x71,0x53,0x1A,0xA3,
        0x82,0xA6,0x8A,0xCC,0x00,0x00,0x00,0x00};
    static const uint8_t kV2Golden[kPageHeaderSize] = {
        0x4F,0x52,0x53,0x31,0x02,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,
        0x0E,0x8A,0xDE,0x7E,0x71,0x53,0x1A,0xA3,
        0x2D,0x0F,0xC7,0x06,0x00,0x00,0x00,0x00};
    uint8_t bytes[kPageHeaderSize];
    const PageHeader header{7, 0x0E8ADE7E71531AA3ULL};
    encodePageHeaderVersion(header, kVersionV1, bytes);
    assert(memcmp(bytes, kV1Golden, sizeof(bytes)) == 0);
    encodePageHeader(header, bytes);
    assert(memcmp(bytes, kV2Golden, sizeof(bytes)) == 0);
  }

  // Erased flash is not a valid header, and headerMagicPresent() correctly
  // reports "nothing here" rather than "unsupported".
  {
    uint8_t bytes[kPageHeaderSize];
    memset(bytes, 0xFF, sizeof(bytes));
    PageHeader decoded{};
    assert(!decodePageHeader(bytes, decoded));
    uint8_t version = 0;
    assert(!headerMagicPresent(bytes, &version));
  }
  // Generation 0 is never valid.
  {
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(PageHeader{0, 1}, bytes);
    PageHeader decoded{};
    assert(!decodePageHeader(bytes, decoded));
  }
  // CRC corruption is detected.
  {
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(PageHeader{5, 9}, bytes);
    bytes[16] ^= 0x01;  // inside device_identity
    PageHeader decoded{};
    assert(!decodePageHeader(bytes, decoded));
  }
  // Torn write: body/CRC landed, commit word still erased.
  {
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(PageHeader{5, 9}, bytes);
    memset(bytes + 28, 0xFF, 4);
    PageHeader decoded{};
    assert(!decodePageHeader(bytes, decoded));
  }
  // Unrecognized/newer version: headerMagicPresent() reports the magic +
  // version so callers can classify this as UNSUPPORTED, but decodePageHeader
  // itself still fails closed rather than misinterpreting the payload.
  {
    uint8_t bytes[kPageHeaderSize];
    encodePageHeader(PageHeader{5, 9}, bytes);
    bytes[4] = kVersion + 1;
    PageHeader decoded{};
    assert(!decodePageHeader(bytes, decoded));
    uint8_t version = 0;
    assert(headerMagicPresent(bytes, &version));
    assert(version == kVersion + 1);
  }

  // ---- Credential record ----
  {
    uint8_t bytes[kCredentialRecordSize];
    Credential original{};
    fillId(original.credential_id, 1);
    original.key_epoch = 3;
    original.device_identity = 0x0E8ADE7E71531AA3ULL;
    fillKRoot(original.k_root, 7);
    encodeCredential(original, bytes);
    Credential decoded{};
    assert(decodeCredential(bytes, decoded));
    assert(memcmp(decoded.credential_id, original.credential_id, kCredentialIdSize) == 0);
    assert(decoded.key_epoch == 3);
    assert(decoded.device_identity == original.device_identity);
    assert(memcmp(decoded.k_root, original.k_root, kKRootSize) == 0);
  }
  // Erased flash is not a valid credential.
  {
    uint8_t bytes[kCredentialRecordSize];
    memset(bytes, 0xFF, sizeof(bytes));
    Credential decoded{};
    assert(!decodeCredential(bytes, decoded));
  }
  // CRC corruption inside K_root is detected.
  {
    uint8_t bytes[kCredentialRecordSize];
    Credential original{};
    fillId(original.credential_id, 2);
    fillKRoot(original.k_root, 4);
    encodeCredential(original, bytes);
    bytes[40] ^= 0x80;  // inside k_root
    Credential decoded{};
    assert(!decodeCredential(bytes, decoded));
  }
  // Torn write: commit word never programmed.
  {
    uint8_t bytes[kCredentialRecordSize];
    Credential original{};
    fillId(original.credential_id, 2);
    fillKRoot(original.k_root, 4);
    encodeCredential(original, bytes);
    memset(bytes + 64, 0xFF, 4);
    Credential decoded{};
    assert(!decodeCredential(bytes, decoded));
  }
  // credentialIdEqual() distinguishes different ids and matches identical ones.
  {
    uint8_t a[kCredentialIdSize], b[kCredentialIdSize];
    fillId(a, 1);
    fillId(b, 1);
    assert(credentialIdEqual(a, b));
    b[0] ^= 1;
    assert(!credentialIdEqual(a, b));
  }

  // ---- TX_RESERVE record ----
  {
    uint8_t bytes[kTxReserveRecordSize];
    TxReserve original{};
    fillId(original.credential_id, 9);
    original.key_epoch = 1;
    original.tx_reserved_bound = kTxReservationBlockSize * 3;
    encodeTxReserve(original, bytes);
    TxReserve decoded{};
    assert(decodeTxReserve(bytes, decoded));
    assert(memcmp(decoded.credential_id, original.credential_id, kCredentialIdSize) == 0);
    assert(decoded.key_epoch == 1);
    assert(decoded.tx_reserved_bound == kTxReservationBlockSize * 3);
  }
  // Bound of zero is never valid (even if otherwise sealed correctly, encode()
  // itself is never asked to produce this, but decode() must still reject a
  // hand-crafted or corrupted zero bound).
  {
    uint8_t bytes[kTxReserveRecordSize];
    TxReserve original{};
    fillId(original.credential_id, 9);
    original.tx_reserved_bound = 0;
    encodeTxReserve(original, bytes);
    TxReserve decoded{};
    assert(!decodeTxReserve(bytes, decoded));
  }
  // A bound that is not a multiple of the reservation block size is
  // structurally impossible from a genuine sequence of reservations and must
  // be rejected as corrupt rather than accepted as a smaller-but-valid bound.
  {
    uint8_t bytes[kTxReserveRecordSize];
    TxReserve original{};
    fillId(original.credential_id, 9);
    original.tx_reserved_bound = kTxReservationBlockSize + 1;
    encodeTxReserve(original, bytes);
    TxReserve decoded{};
    assert(!decodeTxReserve(bytes, decoded));
  }
  // Erased flash is not a valid TX_RESERVE record.
  {
    uint8_t bytes[kTxReserveRecordSize];
    memset(bytes, 0xFF, sizeof(bytes));
    TxReserve decoded{};
    assert(!decodeTxReserve(bytes, decoded));
  }
  // CRC corruption is detected.
  {
    uint8_t bytes[kTxReserveRecordSize];
    TxReserve original{};
    fillId(original.credential_id, 9);
    original.tx_reserved_bound = kTxReservationBlockSize;
    encodeTxReserve(original, bytes);
    bytes[20] ^= 0x01;  // inside tx_reserved_bound
    TxReserve decoded{};
    assert(!decodeTxReserve(bytes, decoded));
  }

  // ---- v2 SECURITY_STATE record ----
  {
    uint8_t bytes[kSecurityStateRecordSize];
    SecurityStateRecord original{};
    fillId(original.credential_id, 21);
    original.key_epoch = 4;
    original.kind = SecurityStateKind::kTxReserveExclusiveBound;
    original.value = kTxReservationBlockSize * 5;
    encodeSecurityState(original, bytes);
    SecurityStateRecord decoded{};
    assert(decodeSecurityState(bytes, decoded));
    assert(memcmp(decoded.credential_id, original.credential_id,
                  kCredentialIdSize) == 0);
    assert(decoded.key_epoch == 4);
    assert(decoded.kind == SecurityStateKind::kTxReserveExclusiveBound);
    assert(decoded.value == original.value);
  }
  {
    uint8_t bytes[kSecurityStateRecordSize];
    SecurityStateRecord original{};
    fillId(original.credential_id, 22);
    original.key_epoch = 9;
    original.kind = SecurityStateKind::kA2dReplayExclusiveBound;
    original.value = kA2dReplayReservationBlockSize * 7;
    encodeSecurityState(original, bytes);
    SecurityStateRecord decoded{};
    assert(decodeSecurityState(bytes, decoded));
    assert(decoded.kind == SecurityStateKind::kA2dReplayExclusiveBound);
    assert(decoded.value == original.value);
  }
  // Exact v2 state-record golden bytes for both authorized kinds.
  {
    static const uint8_t kTxGolden[kSecurityStateRecordSize] = {
        0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,
        0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,
        0x00,0x00,0x00,0x04,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x00,
        0xDF,0x9B,0x2D,0x88,0x00,0x00,0x00,0x00};
    static const uint8_t kA2dGolden[kSecurityStateRecordSize] = {
        0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,
        0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,
        0x00,0x00,0x00,0x09,0x02,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,
        0x3D,0x6B,0xA6,0xF5,0x00,0x00,0x00,0x00};

    SecurityStateRecord state{};
    uint8_t bytes[kSecurityStateRecordSize];
    fillId(state.credential_id, 21);
    state.key_epoch = 4;
    state.kind = SecurityStateKind::kTxReserveExclusiveBound;
    state.value = kTxReservationBlockSize * 5;
    encodeSecurityState(state, bytes);
    assert(memcmp(bytes, kTxGolden, sizeof(bytes)) == 0);

    state = SecurityStateRecord{};
    fillId(state.credential_id, 22);
    state.key_epoch = 9;
    state.kind = SecurityStateKind::kA2dReplayExclusiveBound;
    state.value = kA2dReplayReservationBlockSize * 7;
    encodeSecurityState(state, bytes);
    assert(memcmp(bytes, kA2dGolden, sizeof(bytes)) == 0);
  }

  // Unknown kind, nonzero reserved bytes, impossible alignment, CRC damage and
  // erased/torn commit state all fail closed.
  {
    uint8_t bytes[kSecurityStateRecordSize];
    SecurityStateRecord original{};
    fillId(original.credential_id, 23);
    original.key_epoch = 1;
    original.kind = SecurityStateKind::kTxReserveExclusiveBound;
    original.value = kTxReservationBlockSize;
    encodeSecurityState(original, bytes);

    uint8_t mutated[kSecurityStateRecordSize];
    memcpy(mutated, bytes, sizeof(mutated));
    mutated[20] = 0x7F;
    SecurityStateRecord decoded{};
    assert(!decodeSecurityState(mutated, decoded));

    memcpy(mutated, bytes, sizeof(mutated));
    mutated[21] = 1;
    assert(!decodeSecurityState(mutated, decoded));

    SecurityStateRecord bad = original;
    bad.value = kTxReservationBlockSize + 1;
    encodeSecurityState(bad, mutated);
    assert(!decodeSecurityState(mutated, decoded));

    bad = original;
    bad.kind = SecurityStateKind::kA2dReplayExclusiveBound;
    bad.value = kA2dReplayReservationBlockSize + 1;
    encodeSecurityState(bad, mutated);
    assert(!decodeSecurityState(mutated, decoded));

    memcpy(mutated, bytes, sizeof(mutated));
    mutated[24] ^= 0x01;
    assert(!decodeSecurityState(mutated, decoded));

    memcpy(mutated, bytes, sizeof(mutated));
    memset(mutated + 36, 0xFF, 4);
    assert(!decodeSecurityState(mutated, decoded));

    memset(mutated, 0xFF, sizeof(mutated));
    assert(!decodeSecurityState(mutated, decoded));
  }

  // ---- Page packing sanity ----
  {
    assert(pageHeaderOffset() == 0);
    assert(credentialRecordOffset() == kPageHeaderSize);
    assert(txReserveRecordOffset(0) ==
           kPageHeaderSize + kCredentialRecordSize);
    assert(txReserveRecordOffset(kV1TxReserveSlotsPerPage - 1) +
               kTxReserveRecordSize ==
           4096);
    assert(securityStateRecordOffset(0) ==
           kPageHeaderSize + kCredentialRecordSize);
    assert(securityStateRecordOffset(kSecurityStateSlotsPerPage - 1) +
               kSecurityStateRecordSize ==
           4096 - kSecurityStateTailBytes);
    assert(kSecurityStateTailBytes == 36);
  }

  puts("M7P6F security_format v1/v2 encode/decode checks: PASS");
}
