#pragma once
#include <stddef.h>
#include <stdint.h>

// M7P6B: on-flash durable SecurityStore v1 record format
// (docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md §5-§7,
// docs/milestones/M7P6B.md). A sibling of journal_format.h/config_format.h:
// explicit magic, schema version, generation, DeviceIdentity binding, CRC32
// and a final 4-byte commit word programmed separately last, reusing
// journal_format's already-tested byte-order/CRC32/erased-check primitives
// directly instead of duplicating them. No shared state or physical pages
// with HistoryStore or ConfigStore.
//
// Page layout (storage_config::kPageSize == 4096 bytes each, 2 pages A/B):
//
//   [0 .. kPageHeaderSize)                          page header
//   [kPageHeaderSize .. +kCredentialRecordSize)      exactly one CREDENTIAL slot
//   [.. end of page)                                 kTxReserveSlotsPerPage
//                                                     TX_RESERVE slots
//
// Exactly one CREDENTIAL slot per page is deliberate, not a size accident:
// credential changes are rare (re-provisioning only) and always happen
// together with a fresh page (SecurityStore::provision() always compacts
// onto a brand-new page carrying the new credential plus a freshly reserved
// TX block), so recovery never needs to scan multiple CREDENTIAL candidates
// per page -- slot 0 is the only one that can ever exist.
namespace orun_tlp::security_format {

constexpr uint32_t kMagic = 0x4F525331;  // "ORS1"
constexpr uint8_t kVersion = 1;
constexpr uint32_t kCommit = 0;

// credential_id is a 128-bit random identifier (conventional UUID-class
// width): large enough that accidental collision across any realistic
// number of device re-provisioning events over the product's life is
// negligible, without inventing a bespoke narrower encoding. See
// docs/milestones/M7P6B.md for the exact justification.
constexpr size_t kCredentialIdSize = 16;
// K_root is exactly 256 random bits, stored raw (no at-rest
// encryption/obfuscation is invented here -- physical extraction protection
// is an explicitly unresolved product-security boundary, see the ADR).
constexpr size_t kKRootSize = 32;

// ---- Page header: magic(4) + version+reserved(4) + generation(8) +
// device_identity(8) + crc32(4) + commit(4). ----
constexpr uint32_t kPageHeaderSize = 32;

// ---- CREDENTIAL record: credential_id(16) + key_epoch(4) +
// device_identity(8) + k_root(32) + crc32(4) + commit(4). ----
constexpr uint32_t kCredentialRecordSize = 68;

// ---- TX_RESERVE record: credential_id(16) + key_epoch(4) +
// tx_reserved_bound(8) + crc32(4) + commit(4). ----
constexpr uint32_t kTxReserveRecordSize = 36;

constexpr uint32_t kTxReserveSlotsPerPage =
    (4096 - kPageHeaderSize - kCredentialRecordSize) / kTxReserveRecordSize;

static_assert(kPageHeaderSize + kCredentialRecordSize +
                      kTxReserveSlotsPerPage * kTxReserveRecordSize ==
                  4096,
              "security page must pack exactly into one 4096-byte page");

// Initial TX counter reservation block size (ADR §6 seed value). A separate
// namespace/constant from storage_config::kSequenceBlockSize -- the numeric
// value happens to match today, but this is its own named constant with no
// shared storage, state or code with HistoryStore's sequence reservation.
constexpr uint64_t kTxReservationBlockSize = 256;

uint32_t pageHeaderOffset();
uint32_t credentialRecordOffset();
uint32_t txReserveRecordOffset(unsigned slot);

struct PageHeader {
  // Explicit constructor (not default member initializers): the vendored
  // RAK toolchain builds this firmware under gnu++11, where a struct with
  // default member initializers is not an aggregate and brace-init like
  // PageHeader{a, b} would not compile -- matches config_format::Config's
  // own pattern.
  constexpr PageHeader(uint64_t generation_value = 0, uint64_t device_identity_value = 0)
      : generation(generation_value), device_identity(device_identity_value) {}
  uint64_t generation;
  uint64_t device_identity;
};

// bytes must point at kPageHeaderSize writable/readable bytes.
void encodePageHeader(const PageHeader& header, uint8_t* bytes);
// Fails closed (false) on bad magic, nonzero reserved bytes, zero
// generation, or a CRC/commit mismatch (torn or never-written header).
// Does NOT itself distinguish version mismatch from other decode failure --
// callers that must tell "not a header at all" apart from "a newer,
// unsupported header version" should call headerVersion() first.
bool decodePageHeader(const uint8_t* bytes, PageHeader& header);
// Returns true and sets *version if bytes carry this format's magic at all
// (any version), false if bytes do not look like a security page header
// (wrong magic and not fully erased) or are fully erased/blank. Used to
// distinguish UNSUPPORTED (recognized magic, unrecognized version) from
// ordinary blank/corrupt bytes during recovery.
bool headerMagicPresent(const uint8_t* bytes, uint8_t* version);

struct Credential {
  uint8_t credential_id[kCredentialIdSize]{};
  uint32_t key_epoch = 0;
  uint64_t device_identity = 0;
  uint8_t k_root[kKRootSize]{};
};

// bytes must point at kCredentialRecordSize writable/readable bytes.
void encodeCredential(const Credential& credential, uint8_t* bytes);
// Fails closed on CRC/commit mismatch (torn/never-written record). Does not
// itself validate device_identity binding -- callers compare the decoded
// device_identity against the recovering DeviceIdentity themselves so a
// mismatch can be reported as FOREIGN rather than silently discarded.
bool decodeCredential(const uint8_t* bytes, Credential& credential);
bool credentialIdEqual(const uint8_t (&a)[kCredentialIdSize],
                        const uint8_t (&b)[kCredentialIdSize]);

struct TxReserve {
  uint8_t credential_id[kCredentialIdSize]{};
  uint32_t key_epoch = 0;
  uint64_t tx_reserved_bound = 0;
};

// bytes must point at kTxReserveRecordSize writable/readable bytes.
void encodeTxReserve(const TxReserve& reserve, uint8_t* bytes);
// Fails closed on CRC/commit mismatch, or a bound that is zero or not a
// multiple of kTxReservationBlockSize (a bound can only ever advance in
// whole reservation blocks -- any other value is torn/corrupt, never a
// legitimately smaller-but-valid bound).
bool decodeTxReserve(const uint8_t* bytes, TxReserve& reserve);

}  // namespace orun_tlp::security_format
