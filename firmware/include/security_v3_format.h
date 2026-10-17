#pragma once
#include <stddef.h>
#include <stdint.h>

// SF5K3F: PRODUCT-SOURCE version-3 custody journal byte codec.
// This module performs neither flash I/O nor security authorization.
// SecurityStore still writes only v2. Entry encoding alone grants nothing.
// The explicit v3 page writer/recovery/owner gates are a LATER step.
namespace orun_tlp {
namespace security_v3_format {

constexpr uint8_t kVersion = 3U;
constexpr size_t kPageBytes = 4096U;
constexpr size_t kHeaderBytes = 32U;
constexpr size_t kCredentialBytes = 68U;
constexpr size_t kRecordBytes = 56U;
constexpr size_t kRecordCount =
    (kPageBytes - kHeaderBytes - kCredentialBytes) / kRecordBytes;
constexpr size_t kJournalOffset = kHeaderBytes + kCredentialBytes;
constexpr size_t kTailBytes = kPageBytes - kJournalOffset -
                              kRecordCount * kRecordBytes;
constexpr size_t kJournalBytes = kRecordCount * kRecordBytes + kTailBytes;
constexpr size_t kGatewayCount = 4U;
constexpr size_t kSnapshotCount = 1U + kGatewayCount;
static_assert(kRecordCount == 71U && kTailBytes == 20U,
              "SecurityStore v3 requires existing 4096-byte page geometry");

enum class Kind : uint8_t {
  kTxReserve = 1U,
  kA2dReplayReserve = 2U,
  kCustodyPolicy = 3U,
  kPolicySnapshot = 4U,
  kGatewaySnapshot = 5U
};

// Logical representation of the exact 56-byte v3 journal entry. Neither
// application input nor a received ACK may ever directly authorize this.
struct Entry {
  uint8_t credential_id[16]{};
  uint32_t key_epoch = 0U;
  Kind kind = Kind::kTxReserve;
  uint8_t slot = 0xFFU;
  uint64_t value = 0U;
  uint32_t grant_generation = 0U;
  uint32_t custody_floor = 0U;
  uint32_t policy_revision = 0U;
};

struct GatewaySlot {
  uint64_t gateway_id = 0U;  // 0 = absent or revoked
  uint32_t generation = 0U;  // revoked slots retain nonzero tombstones
};

struct Snapshot {
  uint8_t credential_id[16]{};
  uint32_t key_epoch = 0U;
  uint64_t tx_bound = 0U;
  uint64_t a2d_bound = 0U;
  uint32_t custody_floor = 0U;
  uint32_t policy_revision = 0U;
  GatewaySlot gateways[kGatewayCount]{};
};

// Encode complete CRC + zero commit marker into fixed bytes. A future
// SecurityStore flash writer MUST program/verify [0,52), and then [52,56)
// separately, never issue this entire buffer in a single write.
bool encodeRecord(const Entry& entry, uint8_t (&out)[kRecordBytes]);
bool decodeCommittedRecord(const uint8_t (&raw)[kRecordBytes], Entry& out);

// Five mandatory opening entries, written in exact order. Entire output
// remains unchanged on validation failure. The helper does NOT decide
// credential provisioning or policy authorization.
bool encodeSnapshotPrefix(
    const Snapshot& snapshot,
    uint8_t (&out)[kSnapshotCount][kRecordBytes]);

// Parse exactly the 71*56-byte v3 journal plus erased 20-byte tail.
// Requires a fully committed five-entry prefix even for zero grants,
// matches credential and epoch, verifies CRC, revision/floor ordering,
// global generation high-water and TX/A2D monotonicity.
// Torn pre-commit appends burn their slot; holes and uncertain commits
// fail closed. Returns reconstructed state ONLY on full success.
bool recoverCommittedJournal(const uint8_t* journal, size_t bytes,
                             const uint8_t (&credential_id)[16],
                             uint32_t key_epoch, Snapshot& out);

}  // namespace security_v3_format
}  // namespace orun_tlp
