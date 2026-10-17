#include "security_v3_format.h"

#include <string.h>
#include "journal_format.h"

namespace orun_tlp {
namespace security_v3_format {
namespace {

namespace jf = journal_format;
constexpr size_t kCrcAt = 48U;
constexpr size_t kCommitAt = 52U;

bool validIdentity(const uint8_t* id) {
  bool all_zero = true, all_erased = true;
  for (size_t i = 0U; i < 16U; ++i) {
    all_zero = all_zero && id[i] == 0U;
    all_erased = all_erased && id[i] == 0xFFU;
  }
  return !all_zero && !all_erased;
}

bool sameScope(const Entry& entry, const Snapshot& state) {
  return entry.key_epoch == state.key_epoch &&
         memcmp(entry.credential_id, state.credential_id, 16U) == 0;
}

uint32_t largestGeneration(const Snapshot& s) {
  uint32_t high = 0U;
  for (size_t i = 0U; i < kGatewayCount; ++i)
    if (s.gateways[i].generation > high)
      high = s.gateways[i].generation;
  return high;
}

bool validRecord(const Entry& e) {
  if (!validIdentity(e.credential_id) || e.key_epoch == UINT32_MAX)
    return false;

  if (e.kind == Kind::kTxReserve || e.kind == Kind::kA2dReplayReserve) {
    const uint64_t block = e.kind == Kind::kTxReserve ? 256U : 8U;
    return e.slot == 0xFFU && e.value != 0U &&
           e.value % block == 0U && e.grant_generation == 0U &&
           e.custody_floor == 0U && e.policy_revision == 0U;
  }

  if (e.custody_floor == UINT32_MAX ||
      e.policy_revision == UINT32_MAX)
    return false;
  if (e.kind == Kind::kPolicySnapshot) {
    return e.slot == 0xFFU && e.grant_generation == 0U &&
           e.value < UINT32_MAX;
  }
  if (e.kind == Kind::kGatewaySnapshot) {
    return e.slot < kGatewayCount &&
           e.grant_generation != UINT32_MAX &&
           (e.value == 0U || e.grant_generation != 0U);
  }
  if (e.kind != Kind::kCustodyPolicy || e.policy_revision == 0U)
    return false;
  if (e.slot == 0xFFU)
    return e.value == 0U && e.grant_generation == 0U;
  return e.slot < kGatewayCount && e.grant_generation != 0U &&
         e.grant_generation != UINT32_MAX;
}

bool validSnapshot(const Snapshot& s) {
  if (!validIdentity(s.credential_id) || s.key_epoch == UINT32_MAX ||
      s.custody_floor == UINT32_MAX ||
      s.policy_revision == UINT32_MAX ||
      (s.tx_bound != 0U && s.tx_bound % 256U != 0U) ||
      (s.a2d_bound != 0U && s.a2d_bound % 8U != 0U))
    return false;
  for (size_t i = 0U; i < kGatewayCount; ++i) {
    const GatewaySlot& slot = s.gateways[i];
    if (slot.generation == UINT32_MAX ||
        (slot.gateway_id != 0U && slot.generation == 0U))
      return false;
    for (size_t j = 0U; j < i; ++j) {
      if (slot.gateway_id != 0U &&
          slot.gateway_id == s.gateways[j].gateway_id)
        return false;
      if (slot.generation != 0U &&
          slot.generation == s.gateways[j].generation)
        return false;
    }
  }
  return s.policy_revision != 0U ||
         (s.custody_floor == 0U && largestGeneration(s) == 0U);
}

void makeRecord(const Snapshot& s, Entry& record, Kind kind,
                uint8_t slot) {
  record = Entry{};
  memcpy(record.credential_id, s.credential_id, 16U);
  record.key_epoch = s.key_epoch;
  record.kind = kind;
  record.slot = slot;
}

bool replayAppend(const Entry& entry, Snapshot& state) {
  if (!sameScope(entry, state)) return false;
  Snapshot next = state;
  if (entry.kind == Kind::kTxReserve) {
    if (entry.value <= state.tx_bound) return false;
    next.tx_bound = entry.value;
  } else if (entry.kind == Kind::kA2dReplayReserve) {
    if (entry.value <= state.a2d_bound) return false;
    next.a2d_bound = entry.value;
  } else if (entry.kind == Kind::kCustodyPolicy) {
    if (state.policy_revision == UINT32_MAX ||
        entry.policy_revision != state.policy_revision + 1U ||
        entry.custody_floor < state.custody_floor)
      return false;
    if (entry.slot == 0xFFU) {
      if (entry.custody_floor == state.custody_floor) return false;
    } else {
      if (entry.grant_generation <= largestGeneration(state))
        return false;
      const size_t index = entry.slot;
      if (entry.value == 0U) {
        if (state.gateways[index].gateway_id == 0U) return false;
      } else {
        for (size_t j = 0U; j < kGatewayCount; ++j)
          if (j != index &&
              state.gateways[j].gateway_id == entry.value)
            return false;
      }
      next.gateways[index].gateway_id = entry.value;
      next.gateways[index].generation = entry.grant_generation;
    }
    next.custody_floor = entry.custody_floor;
    next.policy_revision = entry.policy_revision;
  } else {
    // Snapshot records are legal ONLY at fixed prefix positions.
    return false;
  }
  state = next;
  return true;
}

}  // namespace

bool encodeRecord(const Entry& entry, uint8_t (&out)[kRecordBytes]) {
  if (!validRecord(entry)) return false;
  uint8_t encoded[kRecordBytes]{};
  memcpy(encoded, entry.credential_id, 16U);
  jf::put32(encoded + 16U, entry.key_epoch);
  encoded[20] = static_cast<uint8_t>(entry.kind);
  encoded[21] = entry.slot;
  // 22..23 and 44..47 are strictly zero.
  jf::put64(encoded + 24U, entry.value);
  jf::put32(encoded + 32U, entry.grant_generation);
  jf::put32(encoded + 36U, entry.custody_floor);
  jf::put32(encoded + 40U, entry.policy_revision);
  jf::put32(encoded + kCrcAt, jf::crc32(encoded, kCrcAt));
  jf::put32(encoded + kCommitAt, 0U);
  memcpy(out, encoded, sizeof(encoded));
  return true;
}

bool decodeCommittedRecord(const uint8_t (&raw)[kRecordBytes],
                           Entry& out) {
  if (jf::get32(raw + kCommitAt) != 0U ||
      jf::get32(raw + kCrcAt) != jf::crc32(raw, kCrcAt) ||
      raw[22] != 0U || raw[23] != 0U ||
      jf::get32(raw + 44U) != 0U)
    return false;
  Entry decoded{};
  memcpy(decoded.credential_id, raw, 16U);
  decoded.key_epoch = jf::get32(raw + 16U);
  decoded.slot = raw[21];
  decoded.value = jf::get64(raw + 24U);
  decoded.grant_generation = jf::get32(raw + 32U);
  decoded.custody_floor = jf::get32(raw + 36U);
  decoded.policy_revision = jf::get32(raw + 40U);
  switch (raw[20]) {
    case 1U: decoded.kind = Kind::kTxReserve; break;
    case 2U: decoded.kind = Kind::kA2dReplayReserve; break;
    case 3U: decoded.kind = Kind::kCustodyPolicy; break;
    case 4U: decoded.kind = Kind::kPolicySnapshot; break;
    case 5U: decoded.kind = Kind::kGatewaySnapshot; break;
    default: return false;
  }
  if (!validRecord(decoded)) return false;
  out = decoded;
  return true;
}

bool encodeSnapshotPrefix(
    const Snapshot& snapshot,
    uint8_t (&out)[kSnapshotCount][kRecordBytes]) {
  if (!validSnapshot(snapshot)) return false;
  uint8_t staged[kSnapshotCount][kRecordBytes]{};
  Entry meta{};
  makeRecord(snapshot, meta, Kind::kPolicySnapshot, 0xFFU);
  meta.value = largestGeneration(snapshot);
  meta.custody_floor = snapshot.custody_floor;
  meta.policy_revision = snapshot.policy_revision;
  if (!encodeRecord(meta, staged[0])) return false;
  for (size_t i = 0U; i < kGatewayCount; ++i) {
    Entry e{};
    makeRecord(snapshot, e, Kind::kGatewaySnapshot,
               static_cast<uint8_t>(i));
    e.value = snapshot.gateways[i].gateway_id;
    e.grant_generation = snapshot.gateways[i].generation;
    e.custody_floor = snapshot.custody_floor;
    e.policy_revision = snapshot.policy_revision;
    if (!encodeRecord(e, staged[i + 1U])) return false;
  }
  memcpy(out, staged, sizeof(staged));
  return true;
}

bool recoverCommittedJournal(const uint8_t* journal, size_t bytes,
                             const uint8_t (&credential_id)[16],
                             uint32_t key_epoch, Snapshot& out) {
  if (!journal || bytes != kJournalBytes ||
      !validIdentity(credential_id) || key_epoch == UINT32_MAX ||
      !jf::erased(journal + kRecordCount * kRecordBytes, kTailBytes))
    return false;

  Entry prefix[kSnapshotCount]{};
  for (size_t i = 0U; i < kSnapshotCount; ++i) {
    uint8_t raw[kRecordBytes];
    memcpy(raw, journal + i * kRecordBytes, sizeof(raw));
    if (!decodeCommittedRecord(raw, prefix[i]) ||
        memcmp(prefix[i].credential_id, credential_id, 16U) != 0 ||
        prefix[i].key_epoch != key_epoch)
      return false;
  }
  const Entry& meta = prefix[0];
  if (meta.kind != Kind::kPolicySnapshot) return false;
  Snapshot candidate{};
  memcpy(candidate.credential_id, credential_id, 16U);
  candidate.key_epoch = key_epoch;
  candidate.custody_floor = meta.custody_floor;
  candidate.policy_revision = meta.policy_revision;
  for (size_t i = 0U; i < kGatewayCount; ++i) {
    const Entry& slot = prefix[i + 1U];
    if (slot.kind != Kind::kGatewaySnapshot || slot.slot != i ||
        slot.custody_floor != meta.custody_floor ||
        slot.policy_revision != meta.policy_revision)
      return false;
    candidate.gateways[i].gateway_id = slot.value;
    candidate.gateways[i].generation = slot.grant_generation;
  }
  if (!validSnapshot(candidate) ||
      largestGeneration(candidate) != meta.value)
    return false;

  bool seen_erased = false;
  for (size_t i = kSnapshotCount; i < kRecordCount; ++i) {
    const uint8_t* at = journal + i * kRecordBytes;
    if (jf::erased(at, kRecordBytes)) {
      seen_erased = true;
      continue;
    }
    if (seen_erased) return false;
    if (jf::erased(at + kCommitAt, 4U)) {
      // Interrupted pre-commit: no security decision may have used it.
      // Burn the slot; a later correctly committed entry may follow.
      continue;
    }
    uint8_t raw[kRecordBytes];
    memcpy(raw, at, sizeof(raw));
    Entry update{};
    if (!decodeCommittedRecord(raw, update) ||
        !replayAppend(update, candidate))
      return false;
  }
  out = candidate;  // only publish after ALL records and tail validate
  return true;
}

}  // namespace security_v3_format
}  // namespace orun_tlp
