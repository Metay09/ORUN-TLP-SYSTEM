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

  // ---- Page packing sanity ----
  {
    assert(pageHeaderOffset() == 0);
    assert(credentialRecordOffset() == kPageHeaderSize);
    assert(txReserveRecordOffset(0) == kPageHeaderSize + kCredentialRecordSize);
    assert(txReserveRecordOffset(kTxReserveSlotsPerPage - 1) + kTxReserveRecordSize == 4096);
  }

  puts("M7P6B security_format encode/decode checks: PASS");
}
