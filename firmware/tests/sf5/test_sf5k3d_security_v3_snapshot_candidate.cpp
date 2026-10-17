#include "sf5k3d_security_v3_snapshot_candidate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "journal_format.h"
#include "security_format.h"

using namespace orun_tlp;
namespace c = sf5k3c_candidate;
namespace d = sf5k3d_candidate;
namespace jf = journal_format;
namespace sf = security_format;

namespace {
constexpr uint64_t kTracker = 0x0102030405060708ULL;
constexpr uint64_t kGateway = 0x2122232425262700ULL;
constexpr size_t kSnapshotDataSize =
    d::kSnapshotRecords * c::kEntrySize + 2U * c::kEntrySize;
constexpr size_t kStagedBodySize = 28U + c::kCredentialSize + kSnapshotDataSize;
constexpr size_t kCompleteWriteSize = kStagedBodySize + 4U;

c::State seed() {
  c::State state{};
  for (size_t i = 0U; i < 16U; ++i)
    state.credential_id[i] = static_cast<uint8_t>(i + 1U);
  state.key_epoch = 7U;
  state.tx_bound = 768U;
  state.a2d_bound = 24U;
  return state;
}

void root(uint8_t (&key)[32]) {
  for (size_t i = 0U; i < sizeof(key); ++i)
    key[i] = static_cast<uint8_t>(0xA0U + i);
}

c::Entry policy(const c::State& s, uint8_t slot, uint64_t id,
                uint32_t generation, uint32_t floor, uint32_t revision) {
  c::Entry p{};
  memcpy(p.credential_id, s.credential_id, sizeof(p.credential_id));
  p.key_epoch = s.key_epoch;
  p.kind = c::Kind::kCustodyPolicy;
  p.slot = slot;
  p.value = id;
  p.grant_generation = generation;
  p.custody_floor = floor;
  p.policy_revision = revision;
  return p;
}

c::State grants() {
  c::State s = seed();
  for (uint8_t slot = 0U; slot < c::kMaxGateways; ++slot)
    assert(c::applyHostAuthenticated(
        policy(s, slot, kGateway + slot + 1U,
               static_cast<uint32_t>(slot) + 2U, 0U,
               static_cast<uint32_t>(slot) + 1U), true, s));
  assert(c::applyHostAuthenticated(policy(s, 1U, 0U, 6U, 0U, 5U),
                                   true, s));
  assert(c::applyHostAuthenticated(policy(s, 0xFFU, 0U, 0U, 2U, 6U),
                                   true, s));
  return s;
}

void makeV2(const c::State& s, uint64_t generation,
            const uint8_t (&key)[32], uint8_t (&page)[c::kPageSize]) {
  memset(page, 0xFF, sizeof(page));
  uint8_t header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(sf::PageHeader{generation, kTracker},
                              sf::kVersionV2, header);
  memcpy(page, header, sizeof(header));
  sf::Credential cred{};
  memcpy(cred.credential_id, s.credential_id, 16U);
  cred.key_epoch = s.key_epoch;
  cred.device_identity = kTracker;
  memcpy(cred.k_root, key, sizeof(cred.k_root));
  sf::encodeCredential(cred, page + sf::kPageHeaderSize);
  size_t offset = sf::kPageHeaderSize + sf::kCredentialRecordSize;
  for (unsigned kind = 0U; kind < 2U; ++kind) {
    sf::SecurityStateRecord record{};
    memcpy(record.credential_id, s.credential_id, 16U);
    record.key_epoch = s.key_epoch;
    record.kind = kind == 0U ?
        sf::SecurityStateKind::kTxReserveExclusiveBound :
        sf::SecurityStateKind::kA2dReplayExclusiveBound;
    record.value = kind == 0U ? s.tx_bound : s.a2d_bound;
    if (record.value == 0U) continue;
    uint8_t raw[sf::kSecurityStateRecordSize]{};
    sf::encodeSecurityState(record, raw);
    memcpy(page + offset, raw, sizeof(raw));
    offset += sizeof(raw);
  }
}

void writePrefix(uint8_t* target, const uint8_t* complete,
                 size_t cut) {
  memset(target, 0xFF, c::kPageSize);
  if (cut > kCompleteWriteSize) cut = kCompleteWriteSize;
  const size_t header_written = cut < 28U ? cut : 28U;
  memcpy(target, complete, header_written);
  if (cut <= 28U) return;
  cut -= 28U;
  const size_t credential_written =
      cut < c::kCredentialSize ? cut : c::kCredentialSize;
  memcpy(target + c::kHeaderSize, complete + c::kHeaderSize,
         credential_written);
  if (cut <= c::kCredentialSize) return;
  cut -= c::kCredentialSize;
  const size_t data_written =
      cut < kSnapshotDataSize ? cut : kSnapshotDataSize;
  memcpy(target + d::kJournalOffset, complete + d::kJournalOffset,
         data_written);
  if (cut <= kSnapshotDataSize) return;
  cut -= kSnapshotDataSize;
  const size_t commit_written = cut < 4U ? cut : 4U;
  memcpy(target + 28U, complete + 28U, commit_written);
}

void expectEquivalent(const c::State& a, const c::State& b) {
  assert(memcmp(a.credential_id, b.credential_id, 16U) == 0);
  assert(a.key_epoch == b.key_epoch);
  assert(a.tx_bound == b.tx_bound && a.a2d_bound == b.a2d_bound);
  assert(a.policy_revision == b.policy_revision);
  assert(a.custody_floor == b.custody_floor);
  for (size_t i = 0U; i < c::kMaxGateways; ++i) {
    assert(a.gateways[i].gateway_id == b.gateways[i].gateway_id);
    assert(a.gateways[i].generation == b.gateways[i].generation);
  }
}

void testSnapshotAndTombstones() {
  c::State state = grants();
  uint8_t entries[d::kSnapshotRecords][c::kEntrySize]{};
  assert(d::encodeSnapshot(state, entries));
  assert(entries[0][20] == static_cast<uint8_t>(c::Kind::kPolicySnapshot));
  for (size_t i = 0U; i < c::kMaxGateways; ++i)
    assert(entries[i + 1U][20] ==
           static_cast<uint8_t>(c::Kind::kGatewaySnapshot));
  assert(jf::get64(entries[2] + 24U) == 0U); // revoked slot 1
  assert(jf::get32(entries[2] + 32U) == 6U); // retained tombstone

  uint8_t journal[d::kEntryAreaSize];
  memset(journal, 0xFF, sizeof(journal));
  memcpy(journal, entries, sizeof(entries));
  c::State recovered{};
  assert(d::recoverSnapshotJournal(journal, sizeof(journal),
                                   state.credential_id, state.key_epoch,
                                   recovered));
  c::State expected = state;
  expected.tx_bound = 0U; // bound entries not appended yet
  expected.a2d_bound = 0U;
  expectEquivalent(expected, recovered);
  assert(!c::grantScopeMatches(recovered, kGateway + 2U, 2U, 3U));
  assert(c::grantScopeMatches(recovered, kGateway + 1U, 2U, 2U));

  // A torn/absent snapshot slot under an *activated* header is never
  // interpreted as a fresh/empty Gateway grant set.
  for (size_t slot = 0U; slot < d::kSnapshotRecords; ++slot) {
    uint8_t mutated[d::kEntryAreaSize];
    memcpy(mutated, journal, sizeof(mutated));
    memset(mutated + slot * c::kEntrySize, 0xFF, c::kEntrySize);
    c::State unchanged = state;
    unchanged.custody_floor = 17U;
    assert(!d::recoverSnapshotJournal(mutated, sizeof(mutated),
                                      state.credential_id, state.key_epoch,
                                      unchanged));
    assert(unchanged.custody_floor == 17U);
  }
  // A self-consistent CRC is not sufficient to forge policy metadata:
  // the snapshot must also match every slot's exact revision/floor and
  // the largest recorded generation.
  uint8_t malformed[d::kEntryAreaSize];
  memcpy(malformed, journal, sizeof(malformed));
  uint8_t raw[c::kEntrySize]{};
  memcpy(raw, malformed + 2U * c::kEntrySize, sizeof(raw));
  c::Entry mismatch{};
  assert(c::decodeEntry(raw, mismatch));
  mismatch.policy_revision += 1U;
  assert(c::encodeEntry(mismatch, raw));
  memcpy(malformed + 2U * c::kEntrySize, raw, sizeof(raw));
  assert(!d::recoverSnapshotJournal(malformed, sizeof(malformed),
                                    state.credential_id, state.key_epoch,
                                    recovered));
  memcpy(malformed, journal, sizeof(malformed));
  memcpy(raw, malformed, sizeof(raw));
  assert(c::decodeEntry(raw, mismatch));
  mismatch.value -= 1U;
  assert(c::encodeEntry(mismatch, raw));
  memcpy(malformed, raw, sizeof(raw));
  assert(!d::recoverSnapshotJournal(malformed, sizeof(malformed),
                                    state.credential_id, state.key_epoch,
                                    recovered));
  // A normal externally submitted update cannot spoof snapshot kinds.
  assert(!c::applyHostAuthenticated(mismatch, true, recovered));
}

void testMigrationCuts() {
  const c::State old_state = seed();
  uint8_t key[32]{};
  root(key);
  uint8_t old[c::kPageSize]{};
  uint8_t next[c::kPageSize]{};
  makeV2(old_state, 5U, key, old);
  assert(d::makeV3SnapshotPage(kTracker, 6U, old_state, key, next));
  uint8_t pages[2][c::kPageSize]{};
  memcpy(pages[0], old, c::kPageSize);
  c::State view{};
  uint64_t generation = 0U;
  // Every byte cut, including all partially written header-commit bytes.
  for (size_t cut = 0U; cut <= kCompleteWriteSize; ++cut) {
    writePrefix(pages[1], next, cut);
    view = old_state;
    view.tx_bound = 0xF000U;
    generation = 99U;
    const d::BootResult status =
        d::recoverTwoPages(pages, kTracker, view, generation);
    if (cut <= kStagedBodySize) {
      assert(status == d::BootResult::kV2);
      expectEquivalent(view, old_state);
      assert(generation == 5U);
    } else if (cut != kCompleteWriteSize) {
      assert(status == d::BootResult::kFault);
      assert(view.tx_bound == 0xF000U && generation == 99U);
    } else {
      assert(status == d::BootResult::kV3);
      expectEquivalent(view, old_state);
      assert(generation == 6U);
    }
  }
  // Old binary sees committed version 3 as unsupported, not as v2.
  sf::PageHeader old_decoder{};
  assert(!sf::decodePageHeaderVersion(
      pages[1], sf::kVersionV2, old_decoder));
  // Unknown future schema, even staged, closes the downgrade path.
  pages[1][4] = 0x04U;
  assert(d::recoverTwoPages(pages, kTracker, view, generation) ==
         d::BootResult::kUnsupported);
}

void testLegacyV1Migration() {
  // The real SecurityStore still recognizes ORS1/v1. An independent
  // migration must carry the v1 TX-exclusive bound, never manufacture
  // an A2D reserve or an enrolled custody Gateway.
  c::State old = seed();
  old.tx_bound = 512U;
  old.a2d_bound = 0U;
  uint8_t key[32]{};
  root(key);
  uint8_t pages[2][c::kPageSize]{};
  memset(pages, 0xFF, sizeof(pages));
  sf::encodePageHeaderVersion(sf::PageHeader{3U, kTracker},
                              sf::kVersionV1, pages[0]);
  sf::Credential cred{};
  memcpy(cred.credential_id, old.credential_id, 16U);
  cred.key_epoch = old.key_epoch;
  cred.device_identity = kTracker;
  memcpy(cred.k_root, key, sizeof(key));
  sf::encodeCredential(cred, pages[0] + sf::kPageHeaderSize);
  sf::TxReserve reserve{};
  memcpy(reserve.credential_id, old.credential_id, 16U);
  reserve.key_epoch = old.key_epoch;
  reserve.tx_reserved_bound = old.tx_bound;
  sf::encodeTxReserve(
      reserve, pages[0] + sf::kPageHeaderSize + sf::kCredentialRecordSize);
  c::State recovered{};
  uint64_t generation = 0U;
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kV1);
  expectEquivalent(recovered, old);
  assert(generation == 3U);

  uint8_t image[c::kPageSize]{};
  assert(d::makeV3SnapshotPage(kTracker, 4U, old, key, image));
  writePrefix(pages[1], image, kStagedBodySize);
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kV1);
  writePrefix(pages[1], image, kCompleteWriteSize);
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kV3);
  expectEquivalent(recovered, old);
  assert(generation == 4U);
}

void testCompactionAndTornOldErase() {
  c::State held = grants();
  uint8_t key[32]{};
  root(key);
  uint8_t old[c::kPageSize]{};
  uint8_t next[c::kPageSize]{};
  assert(d::makeV3SnapshotPage(kTracker, 10U, held, key, old));
  c::State progressed = held;
  progressed.tx_bound = 1024U;
  progressed.a2d_bound = 32U;
  assert(d::makeV3SnapshotPage(kTracker, 11U, progressed, key, next));
  uint8_t pages[2][c::kPageSize]{};
  memcpy(pages[0], old, sizeof(old));
  c::State result{};
  uint64_t generation = 0U;
  for (size_t cut = 0U; cut <= kCompleteWriteSize; ++cut) {
    writePrefix(pages[1], next, cut);
    result = c::State{};
    generation = 0U;
    const d::BootResult status =
        d::recoverTwoPages(pages, kTracker, result, generation);
    if (cut <= kStagedBodySize) {
      assert(status == d::BootResult::kV3 && generation == 10U);
      expectEquivalent(result, held);
    } else if (cut < kCompleteWriteSize) {
      assert(status == d::BootResult::kFault);
    } else {
      assert(status == d::BootResult::kV3 && generation == 11U);
      expectEquivalent(result, progressed);
      assert(result.gateways[1].gateway_id == 0U &&
             result.gateways[1].generation == 6U);
    }
  }
  // The old-page erase may tear after the new header is committed.
  // Either recover the new policy or fail CLOSED; never select the
  // obsolete page or revive its previously revoked Gateway.
  for (size_t count = 0U; count <= c::kPageSize; ++count) {
    memcpy(pages[0], old, sizeof(old));
    memset(pages[0], 0xFF, count);
    result = c::State{};
    generation = 0U;
    const d::BootResult status =
        d::recoverTwoPages(pages, kTracker, result, generation);
    assert(status == d::BootResult::kFault ||
           (status == d::BootResult::kV3 && generation == 11U));
    if (status == d::BootResult::kV3)
      expectEquivalent(result, progressed);
  }
  // A nominally newer page that secretly reset revocation history or
  // counter bounds cannot be accepted during an A/B overlap.
  c::State rolled_back = seed();
  assert(d::makeV3SnapshotPage(kTracker, 12U, rolled_back, key, next));
  memcpy(pages[0], old, sizeof(old));
  memcpy(pages[1], next, sizeof(next));
  assert(d::recoverTwoPages(pages, kTracker, result, generation) ==
         d::BootResult::kFault);
  assert(d::makeV3SnapshotPage(kTracker, 12U, held, key, next));
  memcpy(pages[1], next, sizeof(next));
  assert(d::recoverTwoPages(pages, kTracker, result, generation) ==
         d::BootResult::kV3);
  // Equal committed generation is ambiguous authority, reject.
  assert(d::makeV3SnapshotPage(kTracker, 10U, held, key, next));
  memcpy(pages[1], next, sizeof(next));
  assert(d::recoverTwoPages(pages, kTracker, result, generation) ==
         d::BootResult::kFault);
}

void testCredentialReplacementDropsOldGrants() {
  // Real SecurityStore::commitCredential accepts a DIFFERENT ID and K_root,
  // even when the new epoch numerically equals the old epoch. v3 must not
  // silently reject a compatible rotation or carry old Gateway grants.
  const c::State old_state = grants();
  c::State renewed = seed();
  renewed.tx_bound = 0U;
  renewed.a2d_bound = 0U;
  for (size_t i = 0U; i < sizeof(renewed.credential_id); ++i)
    renewed.credential_id[i] ^= 0x40U;
  uint8_t old_key[32]{};
  root(old_key);
  uint8_t new_key[32]{};
  memcpy(new_key, old_key, sizeof(new_key));
  new_key[0] ^= 0x11U;
  uint8_t pages[2][c::kPageSize]{};
  assert(d::makeV3SnapshotPage(kTracker, 10U, old_state, old_key, pages[0]));
  assert(d::makeV3SnapshotPage(kTracker, 11U, renewed, new_key, pages[1]));
  c::State result{};
  uint64_t gen = 0U;
  assert(d::recoverTwoPages(pages, kTracker, result, gen) ==
         d::BootResult::kV3);
  assert(gen == 11U);
  expectEquivalent(result, renewed);
  assert(!c::grantScopeMatches(result, kGateway + 1U, 2U, 2U));

  // Same credential ID with altered root must not be interpreted as a
  // compaction and cannot be an independent credential commit.
  assert(d::makeV3SnapshotPage(kTracker, 11U, old_state, new_key, pages[1]));
  assert(d::recoverTwoPages(pages, kTracker, result, gen) ==
         d::BootResult::kFault);

  // Same secret with altered credential ID is also not a valid rekey.
  assert(d::makeV3SnapshotPage(kTracker, 11U, renewed, old_key, pages[1]));
  assert(d::recoverTwoPages(pages, kTracker, result, gen) ==
         d::BootResult::kFault);

  // Even with a new identity/root, old grants or TX bound cannot be copied.
  renewed.gateways[0] = old_state.gateways[0];
  renewed.policy_revision = old_state.policy_revision;
  renewed.custody_floor = old_state.custody_floor;
  assert(d::makeV3SnapshotPage(kTracker, 11U, renewed, new_key, pages[1]));
  assert(d::recoverTwoPages(pages, kTracker, result, gen) ==
         d::BootResult::kFault);
  renewed = seed();
  renewed.tx_bound = 768U;
  renewed.a2d_bound = 0U;
  for (size_t i = 0U; i < sizeof(renewed.credential_id); ++i)
    renewed.credential_id[i] ^= 0x40U;
  assert(d::makeV3SnapshotPage(kTracker, 11U, renewed, new_key, pages[1]));
  assert(d::recoverTwoPages(pages, kTracker, result, gen) ==
         d::BootResult::kFault);
}

void testRejectCorruptCredentialAndSnapshot() {
  const c::State state = grants();
  uint8_t key[32]{};
  root(key);
  uint8_t pages[2][c::kPageSize]{};
  memset(pages, 0xFF, sizeof(pages));
  assert(d::makeV3SnapshotPage(kTracker, 3U, state, key, pages[0]));
  c::State recovered{};
  uint64_t generation = 0U;
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kV3);
  expectEquivalent(recovered, state);
  // Fully committed but broken snapshot must never fall back to unprovisioned.
  pages[0][d::kJournalOffset + 53U] ^= 1U;
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kFault);
  pages[0][d::kJournalOffset + 53U] ^= 1U;
  pages[0][c::kHeaderSize + 4U] ^= 1U;
  assert(d::recoverTwoPages(pages, kTracker, recovered, generation) ==
         d::BootResult::kFault);
}

}  // namespace

int main() {
  testSnapshotAndTombstones();
  testMigrationCuts();
  testLegacyV1Migration();
  testCompactionAndTornOldErase();
  testCredentialReplacementDropsOldGrants();
  testRejectCorruptCredentialAndSnapshot();
  puts("SF5K3D v3 A/B snapshot migration/compaction/byte-cut model: PASS");
}
