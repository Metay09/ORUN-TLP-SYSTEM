#include "sf5k3c_security_v3_candidate.h"

#include <string.h>

#include "journal_format.h"

namespace orun_tlp {
namespace sf5k3c_candidate {
namespace {

namespace jf = journal_format;
constexpr uint32_t kOrsMagic = 0x4F525331U;  // ORS1
constexpr size_t kCrcOffset = 48U;
constexpr size_t kCommitOffset = 52U;
constexpr size_t kHeaderCrcOffset = 24U;
constexpr size_t kHeaderCommitOffset = 28U;

bool credentialNotBlank(const uint8_t* bytes) {
  bool all_zero = true;
  bool all_erased = true;
  for (size_t i = 0U; i < 16U; ++i) {
    all_zero = all_zero && bytes[i] == 0U;
    all_erased = all_erased && bytes[i] == 0xFFU;
  }
  return !all_zero && !all_erased;
}

bool validEntry(const Entry& e) {
  if (!credentialNotBlank(e.credential_id) || e.key_epoch == UINT32_MAX)
    return false;

  if (e.kind == Kind::kTxReserve || e.kind == Kind::kA2dReplayReserve) {
    const uint64_t block = e.kind == Kind::kTxReserve ? 256U : 8U;
    return e.slot == 0xFFU && e.value != 0U &&
           (e.value % block) == 0U &&
           e.grant_generation == 0U && e.custody_floor == 0U &&
           e.policy_revision == 0U;
  }
  if (e.kind == Kind::kPolicySnapshot) {
    // Snapshot metadata: max grant generation in value; the same floor
    // and exact revision are repeated on each subsequent slot entry.
    return e.slot == 0xFFU && e.grant_generation == 0U &&
           e.value < UINT32_MAX && e.custody_floor != UINT32_MAX &&
           e.policy_revision != UINT32_MAX;
  }
  if (e.kind == Kind::kGatewaySnapshot) {
    return e.slot < kMaxGateways &&
           e.custody_floor != UINT32_MAX &&
           e.policy_revision != UINT32_MAX &&
           e.grant_generation != UINT32_MAX &&
           (e.value == 0U || e.grant_generation != 0U);
  }
  if (e.kind != Kind::kCustodyPolicy ||
      e.custody_floor == UINT32_MAX ||
      e.policy_revision == 0U || e.policy_revision == UINT32_MAX)
    return false;
  if (e.slot == 0xFFU)
    return e.value == 0U && e.grant_generation == 0U;
  if (e.slot >= kMaxGateways) return false;
  // An enrollment and a revocation both need a new, nonzero per-slot
  // generation; revoked slots persist that high-water for safe re-enrollment.
  return e.grant_generation != 0U &&
         e.grant_generation != UINT32_MAX;
}

bool sameScope(const Entry& entry, const State& state) {
  return entry.key_epoch == state.key_epoch &&
         memcmp(entry.credential_id, state.credential_id,
                sizeof(entry.credential_id)) == 0;
}

bool commitValid(const uint8_t* bytes, size_t crc_at, size_t commit_at) {
  return jf::get32(bytes + commit_at) == 0U &&
         jf::get32(bytes + crc_at) == jf::crc32(bytes, crc_at);
}

}  // namespace

bool encodeEntry(const Entry& value, uint8_t (&out)[kEntrySize]) {
  if (!validEntry(value)) return false;
  uint8_t tmp[kEntrySize]{};
  memcpy(tmp, value.credential_id, 16U);
  jf::put32(tmp + 16U, value.key_epoch);
  tmp[20] = static_cast<uint8_t>(value.kind);
  tmp[21] = value.slot;
  // 22..23 and 44..47 are reserved and zero.
  jf::put64(tmp + 24U, value.value);
  jf::put32(tmp + 32U, value.grant_generation);
  jf::put32(tmp + 36U, value.custody_floor);
  jf::put32(tmp + 40U, value.policy_revision);
  jf::put32(tmp + kCrcOffset, jf::crc32(tmp, kCrcOffset));
  jf::put32(tmp + kCommitOffset, 0U);
  memcpy(out, tmp, sizeof(tmp));
  return true;
}

bool decodeEntry(const uint8_t (&bytes)[kEntrySize], Entry& out) {
  if (!commitValid(bytes, kCrcOffset, kCommitOffset) ||
      bytes[22] != 0U || bytes[23] != 0U ||
      jf::get32(bytes + 44U) != 0U)
    return false;
  Entry e{};
  memcpy(e.credential_id, bytes, 16U);
  e.key_epoch = jf::get32(bytes + 16U);
  e.slot = bytes[21];
  e.value = jf::get64(bytes + 24U);
  e.grant_generation = jf::get32(bytes + 32U);
  e.custody_floor = jf::get32(bytes + 36U);
  e.policy_revision = jf::get32(bytes + 40U);
  switch (bytes[20]) {
    case 1U: e.kind = Kind::kTxReserve; break;
    case 2U: e.kind = Kind::kA2dReplayReserve; break;
    case 3U: e.kind = Kind::kCustodyPolicy; break;
    case 4U: e.kind = Kind::kPolicySnapshot; break;
    case 5U: e.kind = Kind::kGatewaySnapshot; break;
    default: return false;
  }
  if (!validEntry(e)) return false;
  out = e;
  return true;
}

bool encodePageHeader(uint64_t generation, uint64_t tracker,
                      uint8_t (&out)[kHeaderSize]) {
  if (generation == 0U || tracker == 0U) return false;
  uint8_t tmp[kHeaderSize]{};
  jf::put32(tmp, kOrsMagic);
  tmp[4] = kCandidateVersion;
  jf::put64(tmp + 8U, generation);
  jf::put64(tmp + 16U, tracker);
  jf::put32(tmp + kHeaderCrcOffset,
            jf::crc32(tmp, kHeaderCrcOffset));
  jf::put32(tmp + kHeaderCommitOffset, 0U);
  memcpy(out, tmp, sizeof(tmp));
  return true;
}

bool decodePageHeader(const uint8_t (&bytes)[kHeaderSize],
                      uint64_t tracker, uint64_t& generation) {
  if (tracker == 0U || jf::get32(bytes) != kOrsMagic ||
      bytes[4] != kCandidateVersion ||
      bytes[5] != 0U || bytes[6] != 0U || bytes[7] != 0U ||
      jf::get64(bytes + 16U) != tracker ||
      !commitValid(bytes, kHeaderCrcOffset, kHeaderCommitOffset))
    return false;
  const uint64_t candidate = jf::get64(bytes + 8U);
  if (candidate == 0U) return false;
  generation = candidate;
  return true;
}

bool applyHostAuthenticated(const Entry& record, bool authenticated,
                            State& state) {
  if (!authenticated || !validEntry(record) ||
      !credentialNotBlank(state.credential_id) ||
      state.key_epoch == UINT32_MAX || !sameScope(record, state))
    return false;

  State next = state;
  if (record.kind == Kind::kTxReserve) {
    if (record.value <= state.tx_bound) return false;
    next.tx_bound = record.value;
  } else if (record.kind == Kind::kA2dReplayReserve) {
    if (record.value <= state.a2d_bound) return false;
    next.a2d_bound = record.value;
  } else if (record.kind == Kind::kCustodyPolicy) {
    if (state.policy_revision == UINT32_MAX ||
        record.policy_revision != state.policy_revision + 1U ||
        record.custody_floor < state.custody_floor)
      return false;
    if (record.slot == 0xFFU) {
      // A floor-only record must actually advance the global floor.
      if (record.custody_floor == state.custody_floor) return false;
    } else {
      GatewaySlot& target = next.gateways[record.slot];
      // ACK identity contains Gateway ID + generation, NOT the slot index.
      // A revoked Gateway can later be placed into a different empty slot.
      // Therefore slot-local high-water alone is unsafe: never permit any
      // generation to be reused, even if the Gateway changes slots.
      uint32_t max_generation = 0U;
      for (size_t i = 0U; i < kMaxGateways; ++i)
        if (next.gateways[i].generation > max_generation)
          max_generation = next.gateways[i].generation;
      if (record.grant_generation <= max_generation) return false;
      if (record.value == 0U) {
        if (target.gateway_id == 0U) return false;
        target.gateway_id = 0U;
        target.generation = record.grant_generation;  // durable tombstone
      } else {
        for (size_t i = 0U; i < kMaxGateways; ++i)
          if (i != record.slot &&
              next.gateways[i].gateway_id == record.value)
            return false;
        target.gateway_id = record.value;
        target.generation = record.grant_generation;
      }
    }
    next.custody_floor = record.custody_floor;
    next.policy_revision = record.policy_revision;
  } else {
    // Page snapshots are not externally authenticated policy commands.
    // Only the complete, CRC-verified five-entry boot parser may use them.
    return false;
  }
  state = next;
  return true;
}

bool recoverHostJournal(const uint8_t* bytes, size_t len,
                        const State& initial, State& out) {
  if (!bytes || len != kSlotCount * kEntrySize + kTailBytes ||
      !jf::erased(bytes + kSlotCount * kEntrySize, kTailBytes))
    return false;

  State replayed = initial;
  bool erased_hole = false;
  for (size_t slot = 0U; slot < kSlotCount; ++slot) {
    const uint8_t* const pointer = bytes + slot * kEntrySize;
    if (jf::erased(pointer, kEntrySize)) {
      erased_hole = true;
      continue;
    }
    if (erased_hole) return false;
    uint8_t raw[kEntrySize]{};
    memcpy(raw, pointer, sizeof(raw));
    if (jf::erased(raw + kCommitOffset, 4U)) {
      // Interrupted staged body (no commit word): burn slot. Later
      // committed records may exist because a failed append was skipped.
      continue;
    }
    Entry entry{};
    if (!decodeEntry(raw, entry) ||
        !applyHostAuthenticated(entry, true, replayed))
      return false;
  }
  out = replayed;  // Publish once; all failures leave caller state intact.
  return true;
}

bool grantScopeMatches(const State& state, uint64_t gateway_id,
                       uint32_t floor, uint32_t generation) {
  if (gateway_id == 0U || floor == UINT32_MAX ||
      generation == 0U || generation == UINT32_MAX ||
      floor != state.custody_floor)
    return false;
  for (size_t i = 0U; i < kMaxGateways; ++i)
    if (state.gateways[i].gateway_id == gateway_id &&
        state.gateways[i].generation == generation)
      return true;
  return false;
}

}  // namespace sf5k3c_candidate
}  // namespace orun_tlp
