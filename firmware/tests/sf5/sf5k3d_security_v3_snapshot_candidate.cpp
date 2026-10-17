#include "sf5k3d_security_v3_snapshot_candidate.h"

#include <string.h>
#include "journal_format.h"
#include "security_format.h"

namespace orun_tlp {
namespace sf5k3d_candidate {
namespace jf = journal_format;
namespace sf = security_format;
namespace {

bool identityValid(const uint8_t* id) {
  bool any_nonzero = false, any_not_erased = false;
  for (size_t i = 0U; i < 16U; ++i) {
    any_nonzero |= id[i] != 0U;
    any_not_erased |= id[i] != 0xFFU;
  }
  return any_nonzero && any_not_erased;
}

bool sameId(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 16U) == 0;
}

void makeEntry(const c::State& s, c::Entry& e, c::Kind kind,
               uint8_t slot) {
  e = c::Entry{};
  memcpy(e.credential_id, s.credential_id, 16U);
  e.key_epoch = s.key_epoch;
  e.kind = kind;
  e.slot = slot;
}

bool validSnapshotState(const c::State& s) {
  if (!identityValid(s.credential_id) || s.key_epoch == UINT32_MAX ||
      s.custody_floor == UINT32_MAX ||
      s.policy_revision == UINT32_MAX ||
      (s.tx_bound != 0U && s.tx_bound % 256U != 0U) ||
      (s.a2d_bound != 0U && s.a2d_bound % 8U != 0U))
    return false;
  uint32_t max_generation = 0U;
  for (size_t i = 0U; i < c::kMaxGateways; ++i) {
    const c::GatewaySlot& slot = s.gateways[i];
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
    if (slot.generation > max_generation) max_generation = slot.generation;
  }
  // Untouched policy cannot manufacture nonzero state via a snapshot.
  if (s.policy_revision == 0U &&
      (s.custody_floor != 0U || max_generation != 0U))
    return false;
  return true;
}

bool recoverStateLog(const uint8_t* data, size_t slots, size_t width,
                     size_t first, c::State& state) {
  // The prefix is prevalidated; later entries must be counter reservations
  // or ordinary authenticated policy changes, not snapshot restarts.
  bool found_erased = false;
  for (size_t i = first; i < slots; ++i) {
    const uint8_t* at = data + i * width;
    if (jf::erased(at, width)) {
      found_erased = true;
      continue;
    }
    if (found_erased) return false;
    if (jf::erased(at + c::kEntrySize - 4U, 4U))
      continue;  // torn pre-commit: burned slot, not authority
    uint8_t encoded[c::kEntrySize]{};
    memcpy(encoded, at, sizeof(encoded));
    c::Entry update{};
    if (!c::decodeEntry(encoded, update) ||
        (update.kind != c::Kind::kCustodyPolicy &&
         update.kind != c::Kind::kTxReserve &&
         update.kind != c::Kind::kA2dReplayReserve) ||
        !c::applyHostAuthenticated(update, true, state))
      return false;
  }
  return true;
}

bool recoverV2OrV1Log(const uint8_t* page, uint8_t version,
                      c::State& state) {
  const bool v1 = version == sf::kVersionV1;
  const size_t width = v1 ? sf::kTxReserveRecordSize :
                            sf::kSecurityStateRecordSize;
  const size_t slots = v1 ? sf::kV1TxReserveSlotsPerPage :
                            sf::kSecurityStateSlotsPerPage;
  const size_t pos = sf::kPageHeaderSize + sf::kCredentialRecordSize;
  bool found_erased = false;
  for (size_t i = 0U; i < slots; ++i) {
    const uint8_t* at = page + pos + i * width;
    if (jf::erased(at, width)) {
      found_erased = true;
      continue;
    }
    if (found_erased) return false;
    if (jf::erased(at + width - 4U, 4U)) continue;
    if (v1) {
      sf::TxReserve record{};
      if (!sf::decodeTxReserve(at, record) ||
          !sameId(record.credential_id, state.credential_id) ||
          record.key_epoch != state.key_epoch ||
          record.tx_reserved_bound <= state.tx_bound)
        return false;
      state.tx_bound = record.tx_reserved_bound;
    } else {
      sf::SecurityStateRecord record{};
      if (!sf::decodeSecurityState(at, record) ||
          !sameId(record.credential_id, state.credential_id) ||
          record.key_epoch != state.key_epoch)
        return false;
      if (record.kind == sf::SecurityStateKind::kTxReserveExclusiveBound) {
        if (record.value < state.tx_bound) return false;
        state.tx_bound = record.value;
      } else if (
          record.kind == sf::SecurityStateKind::kA2dReplayExclusiveBound) {
        if (record.value < state.a2d_bound) return false;
        state.a2d_bound = record.value;
      } else {
        return false;
      }
    }
  }
  const size_t tail_start = pos + slots * width;
  return jf::erased(page + tail_start, c::kPageSize - tail_start);
}

struct Found {
  bool present = false;
  uint8_t version = 0U;
  uint64_t generation = 0U;
  c::State state{};
};

enum class Inspection : uint8_t {
  kAbsent, kStaged, kPresent, kUnsupported, kCorrupt
};

Inspection inspect(const uint8_t* page, uint64_t tracker, Found& f) {
  if (jf::erased(page, c::kPageSize)) return Inspection::kAbsent;
  // Match the real SecurityStore downgrade contract: a recognized future
  // ORS1 version is not silently ignored, even if activation is staged.
  uint8_t version = 0U;
  if (sf::headerMagicPresent(page, &version) &&
      version != sf::kVersionV1 && version != sf::kVersionV2 &&
      version != c::kCandidateVersion)
    return Inspection::kUnsupported;

  const bool staged = jf::erased(page + 28U, 4U);
  if (staged) return Inspection::kStaged;

  uint8_t raw_header[c::kHeaderSize]{};
  memcpy(raw_header, page, sizeof(raw_header));
  uint64_t generation = 0U;
  if (version == c::kCandidateVersion) {
    if (!c::decodePageHeader(raw_header, tracker, generation))
      return Inspection::kCorrupt;
  } else if (version == sf::kVersionV1 || version == sf::kVersionV2) {
    sf::PageHeader h{};
    if (!sf::decodePageHeaderVersion(raw_header, version, h) ||
        h.device_identity != tracker)
      return Inspection::kCorrupt;
    generation = h.generation;
  } else {
    return Inspection::kCorrupt;
  }

  sf::Credential cred{};
  if (!sf::decodeCredential(page + sf::kPageHeaderSize, cred) ||
      cred.device_identity != tracker ||
      !identityValid(cred.credential_id) ||
      cred.key_epoch == UINT32_MAX)
    return Inspection::kCorrupt;
  c::State state{};
  memcpy(state.credential_id, cred.credential_id, sizeof(state.credential_id));
  state.key_epoch = cred.key_epoch;
  if (version == c::kCandidateVersion) {
    if (!recoverSnapshotJournal(page + kJournalOffset, kEntryAreaSize,
                                state.credential_id, state.key_epoch, state))
      return Inspection::kCorrupt;
  } else {
    if (!recoverV2OrV1Log(page, version, state))
      return Inspection::kCorrupt;
  }
  f.present = true;
  f.version = version;
  f.generation = generation;
  f.state = state;
  return Inspection::kPresent;
}

uint32_t maxGeneration(const c::State& s) {
  uint32_t max_gen = 0U;
  for (size_t i = 0U; i < c::kMaxGateways; ++i)
    if (s.gateways[i].generation > max_gen)
      max_gen = s.gateways[i].generation;
  return max_gen;
}

bool samePolicy(const c::State& a, const c::State& b) {
  if (a.policy_revision != b.policy_revision ||
      a.custody_floor != b.custody_floor)
    return false;
  for (size_t i = 0U; i < c::kMaxGateways; ++i)
    if (a.gateways[i].gateway_id != b.gateways[i].gateway_id ||
        a.gateways[i].generation != b.gateways[i].generation)
      return false;
  return true;
}
}  // namespace

bool encodeSnapshot(const c::State& state,
                    uint8_t (&records)[kSnapshotRecords][c::kEntrySize]) {
  if (!validSnapshotState(state)) return false;
  uint8_t staged[kSnapshotRecords][c::kEntrySize]{};
  uint32_t max_generation = 0U;
  for (size_t i = 0U; i < c::kMaxGateways; ++i)
    if (state.gateways[i].generation > max_generation)
      max_generation = state.gateways[i].generation;
  c::Entry meta{};
  makeEntry(state, meta, c::Kind::kPolicySnapshot, 0xFFU);
  meta.value = max_generation;
  meta.custody_floor = state.custody_floor;
  meta.policy_revision = state.policy_revision;
  if (!c::encodeEntry(meta, staged[0])) return false;
  for (size_t i = 0U; i < c::kMaxGateways; ++i) {
    c::Entry entry{};
    makeEntry(state, entry, c::Kind::kGatewaySnapshot,
              static_cast<uint8_t>(i));
    entry.value = state.gateways[i].gateway_id;
    entry.grant_generation = state.gateways[i].generation;
    entry.custody_floor = state.custody_floor;
    entry.policy_revision = state.policy_revision;
    if (!c::encodeEntry(entry, staged[i + 1U])) return false;
  }
  memcpy(records, staged, sizeof(staged));
  return true;
}

bool recoverSnapshotJournal(const uint8_t* region, size_t len,
                            const uint8_t (&credential_id)[16],
                            uint32_t epoch, c::State& result) {
  if (!region || len != kEntryAreaSize ||
      !jf::erased(region + c::kSlotCount * c::kEntrySize,
                  c::kTailBytes) ||
      !identityValid(credential_id) || epoch == UINT32_MAX)
    return false;
  c::Entry prefix[kSnapshotRecords]{};
  for (size_t i = 0U; i < kSnapshotRecords; ++i) {
    uint8_t raw[c::kEntrySize]{};
    memcpy(raw, region + i * c::kEntrySize, sizeof(raw));
    if (!c::decodeEntry(raw, prefix[i]) ||
        !sameId(prefix[i].credential_id, credential_id) ||
        prefix[i].key_epoch != epoch)
      return false;
  }
  const c::Entry& meta = prefix[0];
  if (meta.kind != c::Kind::kPolicySnapshot) return false;
  c::State next{};
  memcpy(next.credential_id, credential_id, 16U);
  next.key_epoch = epoch;
  next.custody_floor = meta.custody_floor;
  next.policy_revision = meta.policy_revision;
  for (size_t i = 0U; i < c::kMaxGateways; ++i) {
    const c::Entry& item = prefix[i + 1U];
    if (item.kind != c::Kind::kGatewaySnapshot ||
        item.slot != i ||
        item.policy_revision != meta.policy_revision ||
        item.custody_floor != meta.custody_floor)
      return false;
    next.gateways[i].gateway_id = item.value;
    next.gateways[i].generation = item.grant_generation;
  }
  if (!validSnapshotState(next) || maxGeneration(next) != meta.value)
    return false;
  if (!recoverStateLog(region, c::kSlotCount, c::kEntrySize,
                       kSnapshotRecords, next))
    return false;
  result = next;
  return true;
}

bool makeV3SnapshotPage(uint64_t tracker_id, uint64_t page_generation,
                        const c::State& state, const uint8_t (&root)[32],
                        uint8_t (&image)[c::kPageSize]) {
  if (tracker_id == 0U || page_generation == 0U) return false;
  uint8_t snapshot[kSnapshotRecords][c::kEntrySize]{};
  if (!encodeSnapshot(state, snapshot)) return false;
  uint8_t candidate[c::kPageSize];
  memset(candidate, 0xFF, sizeof(candidate));
  uint8_t header[c::kHeaderSize]{};
  if (!c::encodePageHeader(page_generation, tracker_id, header))
    return false;
  memcpy(candidate, header, sizeof(header));
  sf::Credential credential{};
  memcpy(credential.credential_id, state.credential_id, 16U);
  credential.key_epoch = state.key_epoch;
  credential.device_identity = tracker_id;
  memcpy(credential.k_root, root, sizeof(credential.k_root));
  sf::encodeCredential(credential, candidate + c::kHeaderSize);
  memcpy(candidate + kJournalOffset, snapshot, sizeof(snapshot));
  size_t index = kSnapshotRecords;
  for (unsigned kind = 0U; kind < 2U; ++kind) {
    const uint64_t bound = kind == 0U ? state.tx_bound : state.a2d_bound;
    if (bound == 0U) continue;
    c::Entry e{};
    makeEntry(state, e, kind == 0U ? c::Kind::kTxReserve :
                                       c::Kind::kA2dReplayReserve, 0xFFU);
    e.value = bound;
    uint8_t raw[c::kEntrySize]{};
    if (!c::encodeEntry(e, raw)) return false;
    memcpy(candidate + kJournalOffset + index * c::kEntrySize,
           raw, sizeof(raw));
    ++index;
  }
  memcpy(image, candidate, sizeof(candidate));
  return true;
}

BootResult recoverTwoPages(const uint8_t (&pages)[2][c::kPageSize],
                           uint64_t tracker_id, c::State& out,
                           uint64_t& selected_generation) {
  if (tracker_id == 0U) return BootResult::kFault;
  Found found[2]{};
  Inspection classification[2]{};
  for (size_t i = 0U; i < 2U; ++i) {
    classification[i] = inspect(pages[i], tracker_id, found[i]);
    if (classification[i] == Inspection::kUnsupported)
      return BootResult::kUnsupported;
    if (classification[i] == Inspection::kCorrupt)
      return BootResult::kFault;
  }
  int chosen = -1;
  for (size_t i = 0U; i < 2U; ++i) {
    if (!found[i].present) continue;
    if (chosen == -1 ||
        found[i].generation > found[static_cast<size_t>(chosen)].generation)
      chosen = static_cast<int>(i);
    else if (found[i].generation ==
             found[static_cast<size_t>(chosen)].generation)
      return BootResult::kFault;
  }
  if (chosen < 0) return BootResult::kUnprovisioned;
  const size_t winning = static_cast<size_t>(chosen);
  const size_t other = 1U - winning;
  const Found& selected = found[winning];
  if (found[other].present) {
    const Found& older = found[other];
    if (sameId(older.state.credential_id, selected.state.credential_id) &&
        older.state.key_epoch == selected.state.key_epoch) {
      // A/B overlap from a compaction or v2->v3 migration may only advance
      // durable counters and policy scope. Same revision means byte-identical
      // grants, including tombstones. Never erase a revocation by copying
      // an obsolete snapshot with a larger page generation.
      if (selected.state.tx_bound < older.state.tx_bound ||
          selected.state.a2d_bound < older.state.a2d_bound ||
          selected.state.policy_revision < older.state.policy_revision ||
          selected.state.custody_floor < older.state.custody_floor ||
          maxGeneration(selected.state) < maxGeneration(older.state) ||
          (selected.state.policy_revision == older.state.policy_revision &&
           !samePolicy(selected.state, older.state)))
        return BootResult::kFault;
    } else if (selected.state.key_epoch <= older.state.key_epoch) {
      // A new credential epoch cannot go backwards while both A/B
      // generations remain. Old credential keys and grants never migrate.
      return BootResult::kFault;
    }
  }
  out = selected.state;
  selected_generation = selected.generation;
  return selected.version == c::kCandidateVersion ?
      BootResult::kV3 : BootResult::kV2;
}

}  // namespace sf5k3d_candidate
}  // namespace orun_tlp
