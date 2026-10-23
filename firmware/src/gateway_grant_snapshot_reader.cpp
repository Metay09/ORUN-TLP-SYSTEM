#include "gateway_grant_snapshot_reader.h"

#include <string.h>

#include "observation_store_format.h"

namespace orun_tlp {
namespace {
namespace fmt = gateway_grant_format;

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p) {
  uint64_t value = 0U;
  for (unsigned i = 0U; i < 8U; ++i) value = (value << 8U) | p[i];
  return value;
}
void wipe(void* pointer, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(pointer);
  while (length-- != 0U) *p++ = 0U;
}
bool allZero(const uint8_t* bytes, size_t length) {
  uint8_t any = 0U;
  for (size_t i = 0U; i < length; ++i) any |= bytes[i];
  return any == 0U;
}
}  // namespace

GatewayGrantSnapshotReader::~GatewayGrantSnapshotReader() {
  wipeScratch();
}
void GatewayGrantSnapshotReader::wipeScratch() {
  wipe(scratch_, sizeof(scratch_));
}
void GatewayGrantSnapshotReader::denyForCurrentBoot() {
  denied_ = true;
  wipeScratch();
}

bool GatewayGrantSnapshotReader::erasedPage(uint16_t page) const {
  uint8_t block[64]{};
  const uint32_t base = uint32_t(page) * fmt::kPageBytes;
  for (uint32_t off = 0U; off < fmt::kPageBytes; off += sizeof(block)) {
    if (!flash_.read(base + off, block, sizeof(block))) return false;
    for (size_t i = 0U; i < sizeof(block); ++i)
      if (block[i] != 0xFFU) return false;
  }
  return true;
}

bool GatewayGrantSnapshotReader::validateSlots() const {
  for (size_t i = 0U; i < fmt::kGrantSlots; ++i) {
    const uint8_t* s = scratch_ + fmt::kHeaderBytes + i * fmt::kGrantBytes;
    if (allZero(s, fmt::kGrantBytes)) continue;
    const uint64_t tracker = be64(s);
    const uint32_t epoch = be32(s + 8U);
    const uint32_t floor = be32(s + 12U);
    const uint32_t generation = be32(s + 16U);
    const uint32_t flags = be32(s + 20U);
    if (tracker == 0U || tracker == UINT64_MAX ||
        tracker == gateway_id_ || epoch == UINT32_MAX ||
        floor == UINT32_MAX || generation == 0U ||
        generation == UINT32_MAX || flags != 1U ||
        allZero(s + 24U, 32U)) return false;

    for (size_t j = 0U; j < i; ++j) {
      const uint8_t* previous =
          scratch_ + fmt::kHeaderBytes + j * fmt::kGrantBytes;
      if (be64(previous) == tracker) return false;
    }
  }
  return true;
}

bool GatewayGrantSnapshotReader::scanPage(
    uint16_t page, bool& present, uint64_t& generation) {
  present = false;
  generation = 0U;
  const uint32_t base = uint32_t(page) * fmt::kPageBytes;
  uint8_t magic[4]{};
  if (!flash_.read(base, magic, sizeof(magic))) return false;

  if (be32(magic) == UINT32_MAX)
    return erasedPage(page);  // A partially erased page is never blank.
  if (be32(magic) != fmt::kMagic) return false;  // ORS1, torn, unknown.

  if (!flash_.read(base, scratch_, sizeof(scratch_))) return false;
  if (be32(scratch_) != fmt::kMagic ||
      scratch_[4] != fmt::kVersion ||
      scratch_[5] != fmt::kGrantSlots ||
      scratch_[6] != 0U || scratch_[7] != 0U ||
      be64(scratch_ + 8U) != gateway_id_ ||
      be64(scratch_ + 16U) == 0U ||
      be64(scratch_ + 16U) == UINT64_MAX ||
      be32(scratch_ + 24U) == 0U ||
      be32(scratch_ + 24U) == UINT32_MAX ||
      !allZero(scratch_ + 28U, 4U) ||
      be32(scratch_ + fmt::kCommitOffset) != 0U ||
      be32(scratch_ + fmt::kCrcOffset) !=
          observation_store_format::crc32(scratch_, fmt::kCrcOffset) ||
      !validateSlots())
    return false;

  uint8_t proof[fmt::kProofBytes]{};
  memcpy(proof, scratch_ + fmt::kPayloadBytes, sizeof(proof));
  const bool authorized =
      verifier_.verify(scratch_, fmt::kPayloadBytes, proof);
  wipe(proof, sizeof(proof));
  if (!authorized) return false;

  // An authenticated prefix is insufficient when the remaining NOR bytes
  // are nonempty, which may indicate an incompatible/torn page.
  uint8_t remainder[64]{};
  for (uint32_t off = fmt::kStoredBytes; off < fmt::kPageBytes;
       off += sizeof(remainder)) {
    const size_t n = (fmt::kPageBytes - off) < sizeof(remainder)
                         ? fmt::kPageBytes - off : sizeof(remainder);
    if (!flash_.read(base + off, remainder, n)) return false;
    for (size_t i = 0U; i < n; ++i)
      if (remainder[i] != 0xFFU) return false;
  }

  present = true;
  generation = be64(scratch_ + 16U);
  return true;
}

GatewayGrantSnapshotReader::Status GatewayGrantSnapshotReader::begin(
    uint64_t local_gateway_id, bool gateway_security_owner,
    bool tracker_security_store_mounted) {
  if (status_ != Status::kUnopened) return status_;
  if (!gateway_security_owner || tracker_security_store_mounted ||
      local_gateway_id == 0U || local_gateway_id == UINT64_MAX)
    return status_ = Status::kWrongOwner;
  gateway_id_ = local_gateway_id;
  if (!flash_.begin() || flash_.hasUnreconciledMutation())
    return status_ = Status::kUnavailable;

  uint16_t selected_page = UINT16_MAX;
  uint64_t selected_generation = 0U;
  for (uint16_t page = 0U; page < fmt::kPageCount; ++page) {
    bool present = false;
    uint64_t generation = 0U;
    const bool sound = scanPage(page, present, generation);
    wipeScratch();
    if (!sound) return status_ = Status::kRejected;
    if (!present) continue;
    if (generation == selected_generation)
      return status_ = Status::kRejected;
    if (generation > selected_generation) {
      selected_page = page;
      selected_generation = generation;
    }
  }
  if (selected_page == UINT16_MAX)
    return status_ = Status::kEmpty;

  // Bind all future grant lookups to the exact immutable RAM snapshot which
  // passed issuer proof verification. A later unexpected flash change must
  // NOT substitute key bytes without re-authenticating them.
  bool confirmed = false;
  uint64_t confirmed_generation = 0U;
  if (!scanPage(selected_page, confirmed, confirmed_generation) ||
      !confirmed || confirmed_generation != selected_generation) {
    wipeScratch();
    return status_ = Status::kRejected;
  }
  active_page_ = selected_page;
  active_generation_ = selected_generation;
  return status_ = Status::kReady;
}

bool GatewayGrantSnapshotReader::verifiedForLocalGateway(
    uint64_t gateway_id) const {
  return status_ == Status::kReady && !denied_ &&
         active_page_ < fmt::kPageCount && active_generation_ != 0U &&
         gateway_id_ != 0U && gateway_id == gateway_id_ &&
         !flash_.hasUnreconciledMutation();
}

bool GatewayGrantSnapshotReader::activeGrant(
    uint64_t tracker_id, uint32_t key_epoch, GatewayCustodyAckScope& scope,
    uint8_t (&scoped_key)[32]) {
  if (!verifiedForLocalGateway(gateway_id_) ||
      tracker_id == 0U || key_epoch == UINT32_MAX)
    return false;

  // Read ONLY the immutable, issuer-authenticated boot snapshot kept in
  // RAM. Never accept unverified flash bytes after the signed boot scan.
  // Any future writer must denyForCurrentBoot() before mutation.
  for (size_t slot = 0U; slot < fmt::kGrantSlots; ++slot) {
    const uint8_t* candidate =
        scratch_ + fmt::kHeaderBytes + slot * fmt::kGrantBytes;
    if (be64(candidate) == tracker_id &&
        be32(candidate + 8U) == key_epoch &&
        be32(candidate + 20U) == 1U) {
      GatewayCustodyAckScope result{};
      result.tracker_device_id = tracker_id;
      result.gateway_device_id = gateway_id_;
      result.key_epoch = key_epoch;
      result.custody_floor = be32(candidate + 12U);
      result.grant_generation = be32(candidate + 16U);
      memcpy(scoped_key, candidate + 24U, sizeof(scoped_key));
      scope = result;
      return true;
    }
  }
  return false;
}

}  // namespace orun_tlp
