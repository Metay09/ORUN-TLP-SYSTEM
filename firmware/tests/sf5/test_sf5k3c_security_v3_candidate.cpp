#include "sf5k3c_security_v3_candidate.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "journal_format.h"
#include "security_format.h"

using namespace orun_tlp;
namespace c = orun_tlp::sf5k3c_candidate;
namespace jf = orun_tlp::journal_format;
namespace sf = orun_tlp::security_format;

namespace {
constexpr uint64_t kTracker = 0x0102030405060708ULL;
constexpr uint64_t kGatewayBase = 0x1100000000000000ULL;

c::State initial() {
  c::State s{};
  for (size_t i = 0U; i < 16U; ++i)
    s.credential_id[i] = static_cast<uint8_t>(i + 1U);
  s.key_epoch = 7U;
  return s;
}

c::Entry policy(const c::State& scope, uint8_t slot, uint64_t gateway,
                uint32_t generation, uint32_t floor, uint32_t revision) {
  c::Entry e{};
  memcpy(e.credential_id, scope.credential_id, 16U);
  e.key_epoch = scope.key_epoch;
  e.kind = c::Kind::kCustodyPolicy;
  e.slot = slot;
  e.value = gateway;
  e.grant_generation = generation;
  e.custody_floor = floor;
  e.policy_revision = revision;
  return e;
}

c::Entry bound(const c::State& scope, c::Kind kind, uint64_t value) {
  c::Entry e{};
  memcpy(e.credential_id, scope.credential_id, 16U);
  e.key_epoch = scope.key_epoch;
  e.kind = kind;
  e.value = value;
  return e;
}

void testCodecFailClosed() {
  static_assert(c::kSlotCount == 71U && c::kTailBytes == 20U,
                "candidate page arithmetic");
  const c::State scope = initial();
  const c::Entry g = policy(scope, 0U, kGatewayBase + 1U, 4U, 1U, 1U);
  uint8_t encoded[c::kEntrySize]{};
  assert(c::encodeEntry(g, encoded));
  assert(jf::get32(encoded + 52U) == 0U);
  assert(jf::get32(encoded + 48U) == jf::crc32(encoded, 48U));
  c::Entry decoded{};
  assert(c::decodeEntry(encoded, decoded));
  assert(decoded.value == g.value &&
         decoded.grant_generation == g.grant_generation &&
         decoded.custody_floor == 1U &&
         decoded.policy_revision == 1U);

  // Corrupting any of the committed 56 bytes cannot be silently accepted.
  for (size_t offset = 0U; offset < c::kEntrySize; ++offset) {
    uint8_t damaged[c::kEntrySize]{};
    memcpy(damaged, encoded, sizeof(damaged));
    damaged[offset] ^= 0x01U;
    c::Entry unchanged{};
    unchanged.value = UINT64_C(0xA5A5A5A5);
    assert(!c::decodeEntry(damaged, unchanged));
    assert(unchanged.value == UINT64_C(0xA5A5A5A5));
  }
  // Proper CRC cannot make reserved bytes or an unknown kind acceptable.
  uint8_t damaged[c::kEntrySize]{};
  memcpy(damaged, encoded, sizeof(damaged));
  damaged[22] = 1U;
  jf::put32(damaged + 48U, jf::crc32(damaged, 48U));
  assert(!c::decodeEntry(damaged, decoded));
  memcpy(damaged, encoded, sizeof(damaged));
  damaged[20] = 0x99U;
  jf::put32(damaged + 48U, jf::crc32(damaged, 48U));
  assert(!c::decodeEntry(damaged, decoded));

  c::Entry invalid = g;
  invalid.custody_floor = 0U;
  memset(damaged, 0xA5, sizeof(damaged));
  assert(!c::encodeEntry(invalid, damaged));
  for (size_t i = 0U; i < sizeof(damaged); ++i)
    assert(damaged[i] == 0xA5U);

  uint8_t header[c::kHeaderSize]{};
  assert(c::encodePageHeader(1U, kTracker, header));
  uint64_t generation = 0U;
  assert(c::decodePageHeader(header, kTracker, generation) &&
         generation == 1U);
  assert(!c::decodePageHeader(header, kTracker + 1U, generation));
  sf::PageHeader v2_view{};
  assert(!sf::decodePageHeaderVersion(header, sf::kVersionV2, v2_view));
  // The old v2 decoder cannot consume the proposed v3 page.
  uint8_t staged[c::kHeaderSize]{};
  memcpy(staged, header, sizeof(staged));
  memset(staged + 28U, 0xFF, 4U);
  assert(!c::decodePageHeader(staged, kTracker, generation));
}

void testGrantScopeAndCounters() {
  c::State s = initial();
  // No provisioned grant is never a valid ACK authority.
  assert(!c::grantScopeMatches(s, kGatewayBase + 1U, 0U, 1U));
  assert(!c::applyHostAuthenticated(
      policy(s, 0U, kGatewayBase + 1U, 2U, 1U, 1U), false, s));
  assert(s.policy_revision == 0U);

  for (uint8_t i = 0U; i < 4U; ++i) {
    const c::Entry update = policy(
        s, i, kGatewayBase + static_cast<uint64_t>(i) + 1U,
        static_cast<uint32_t>(i) + 2U,
        static_cast<uint32_t>(i) + 1U,
        static_cast<uint32_t>(i) + 1U);
    assert(c::applyHostAuthenticated(update, true, s));
    assert(s.custody_floor == static_cast<uint32_t>(i) + 1U);
  }
  assert(c::grantScopeMatches(s, kGatewayBase + 1U, 4U, 2U));
  assert(c::grantScopeMatches(s, kGatewayBase + 4U, 4U, 5U));
  assert(!c::grantScopeMatches(s, kGatewayBase + 1U, 1U, 2U));
  assert(!c::grantScopeMatches(s, kGatewayBase + 1U, 4U, 3U));
  assert(!c::applyHostAuthenticated(
      policy(s, 4U, kGatewayBase + 5U, 7U, 5U, 5U), true, s));
  assert(!c::applyHostAuthenticated(
      policy(s, 1U, kGatewayBase + 1U, 7U, 5U, 5U), true, s));
  assert(!c::applyHostAuthenticated(
      policy(s, 1U, kGatewayBase + 9U, 7U, 3U, 5U), true, s));
  assert(!c::applyHostAuthenticated(
      policy(s, 1U, kGatewayBase + 9U, 7U, 5U, 4U), true, s));
  const c::Entry revoke = policy(s, 1U, 0U, 0U, 5U, 5U);
  assert(c::applyHostAuthenticated(revoke, true, s));
  assert(!c::grantScopeMatches(s, kGatewayBase + 2U, 5U, 3U));
  assert(!c::grantScopeMatches(s, kGatewayBase + 4U, 4U, 5U));
  assert(c::grantScopeMatches(s, kGatewayBase + 4U, 5U, 5U));
  assert(c::applyHostAuthenticated(
      policy(s, 1U, kGatewayBase + 2U, 6U, 6U, 6U), true, s));
  assert(!c::grantScopeMatches(s, kGatewayBase + 2U, 6U, 3U));
  assert(c::grantScopeMatches(s, kGatewayBase + 2U, 6U, 6U));

  // Rekey under a different credential/epoch never inherits this decision.
  c::Entry wrong_scope = policy(s, 2U, kGatewayBase + 10U, 8U, 7U, 7U);
  wrong_scope.key_epoch = 8U;
  assert(!c::applyHostAuthenticated(wrong_scope, true, s));
  wrong_scope.key_epoch = 7U;
  wrong_scope.credential_id[0] ^= 1U;
  assert(!c::applyHostAuthenticated(wrong_scope, true, s));

  const c::Entry tx = bound(s, c::Kind::kTxReserve, 512U);
  const c::Entry rx = bound(s, c::Kind::kA2dReplayReserve, 16U);
  assert(c::applyHostAuthenticated(tx, true, s));
  assert(c::applyHostAuthenticated(rx, true, s));
  assert(s.tx_bound == 512U && s.a2d_bound == 16U);
  assert(!c::applyHostAuthenticated(tx, true, s));
  assert(!c::applyHostAuthenticated(bound(s, c::Kind::kTxReserve, 256U),
                                     true, s));
  assert(!c::applyHostAuthenticated(bound(s, c::Kind::kA2dReplayReserve, 8U),
                                     true, s));
  assert(s.tx_bound == 512U && s.a2d_bound == 16U);
}

void testPowerCutsAndBootReplay() {
  const c::State seed = initial();
  const c::Entry first = policy(seed, 0U, kGatewayBase + 1U, 3U, 1U, 1U);
  const c::Entry second = policy(seed, 1U, kGatewayBase + 2U, 4U, 2U, 2U);
  uint8_t first_wire[c::kEntrySize]{};
  uint8_t second_wire[c::kEntrySize]{};
  assert(c::encodeEntry(first, first_wire));
  assert(c::encodeEntry(second, second_wire));
  constexpr size_t kRegionSize = c::kSlotCount * c::kEntrySize + c::kTailBytes;
  uint8_t journal[kRegionSize]{};
  for (size_t cut = 0U; cut <= c::kEntrySize; ++cut) {
    memset(journal, 0xFF, sizeof(journal));
    memcpy(journal, first_wire, sizeof(first_wire));
    memcpy(journal + c::kEntrySize, second_wire, cut);

    c::State recovered = seed;
    recovered.custody_floor = 77U; // outputs must not change on rejection
    const bool ok =
        c::recoverHostJournal(journal, sizeof(journal), seed, recovered);
    if (cut <= 52U) {
      assert(ok);
      assert(recovered.custody_floor == 1U);
      assert(recovered.policy_revision == 1U);
      assert(!c::grantScopeMatches(recovered, kGatewayBase + 2U, 2U, 4U));
    } else if (cut == c::kEntrySize) {
      assert(ok);
      assert(recovered.custody_floor == 2U);
      assert(c::grantScopeMatches(recovered, kGatewayBase + 2U, 2U, 4U));
    } else {
      // A partly programmed COMMIT word is ambiguous; fail closed.
      assert(!ok && recovered.custody_floor == 77U);
    }
  }

  // A torn *uncommitted* slot can be burnt; a subsequent verified commit
  // may occupy the following slot, preserving monotonic revision and floor.
  memset(journal, 0xFF, sizeof(journal));
  memcpy(journal, first_wire, sizeof(first_wire));
  memcpy(journal + c::kEntrySize, second_wire, 27U);
  memcpy(journal + 2U * c::kEntrySize, second_wire,
         sizeof(second_wire));
  c::State replayed{};
  assert(c::recoverHostJournal(journal, sizeof(journal), seed, replayed));
  assert(replayed.policy_revision == 2U &&
         replayed.custody_floor == 2U);
  // A true empty hole before later committed state is NOT a burned slot.
  memset(journal + c::kEntrySize, 0xFF, c::kEntrySize);
  assert(!c::recoverHostJournal(journal, sizeof(journal), seed, replayed));
  // Unknown writes in the v3 reserved tail cause fail-closed recovery.
  memset(journal + 2U * c::kEntrySize, 0xFF, c::kEntrySize);
  journal[sizeof(journal) - 1U] = 0U;
  assert(!c::recoverHostJournal(journal, sizeof(journal), seed, replayed));

  // Committed but invalid CRC or stale floor must NEVER be skipped.
  memset(journal, 0xFF, sizeof(journal));
  memcpy(journal, first_wire, sizeof(first_wire));
  journal[24] ^= 1U;
  assert(!c::recoverHostJournal(journal, sizeof(journal), seed, replayed));
  memcpy(journal, first_wire, sizeof(first_wire));
  c::Entry stale = policy(seed, 1U, kGatewayBase + 2U, 4U, 1U, 2U);
  uint8_t stale_wire[c::kEntrySize]{};
  assert(c::encodeEntry(stale, stale_wire));
  memcpy(journal + c::kEntrySize, stale_wire, sizeof(stale_wire));
  assert(!c::recoverHostJournal(journal, sizeof(journal), seed, replayed));
}

void testMigrationSnapshotBoundary() {
  // This is a format *model*, not a SecurityStore migration implementation.
  // Existing v2 headers continue to decode v2; proposed v3 headers fail v2.
  uint8_t old_header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(sf::PageHeader{9U, kTracker},
                              sf::kVersionV2, old_header);
  sf::PageHeader parsed{};
  assert(sf::decodePageHeaderVersion(old_header, sf::kVersionV2, parsed));
  assert(parsed.generation == 9U);

  uint8_t new_header[c::kHeaderSize]{};
  assert(c::encodePageHeader(10U, kTracker, new_header));
  assert(!sf::decodePageHeaderVersion(new_header, sf::kVersionV2, parsed));
  uint64_t next_generation = 0U;
  assert(c::decodePageHeader(new_header, kTracker, next_generation));
  assert(next_generation == 10U);

  const c::State seed = initial();
  sf::Credential current{};
  memcpy(current.credential_id, seed.credential_id, 16U);
  current.key_epoch = seed.key_epoch;
  current.device_identity = kTracker;
  memset(current.k_root, 0x31, sizeof(current.k_root));
  uint8_t credential_bytes[sf::kCredentialRecordSize]{};
  sf::encodeCredential(current, credential_bytes);
  sf::Credential decoded{};
  assert(sf::decodeCredential(credential_bytes, decoded));
  assert(decoded.key_epoch == seed.key_epoch &&
         decoded.device_identity == kTracker);

  // An activated v3 snapshot has the preceding v2 exclusive bounds (not 0).
  constexpr size_t kRegionSize = c::kSlotCount * c::kEntrySize + c::kTailBytes;
  uint8_t region[kRegionSize]{};
  memset(region, 0xFF, sizeof(region));
  uint8_t record[c::kEntrySize]{};
  assert(c::encodeEntry(bound(seed, c::Kind::kTxReserve, 768U), record));
  memcpy(region, record, sizeof(record));
  assert(c::encodeEntry(bound(seed, c::Kind::kA2dReplayReserve, 24U), record));
  memcpy(region + c::kEntrySize, record, sizeof(record));
  c::State snapshot{};
  assert(c::recoverHostJournal(region, sizeof(region), seed, snapshot));
  assert(snapshot.tx_bound == 768U && snapshot.a2d_bound == 24U);
  assert(snapshot.custody_floor == 0U &&
         !c::grantScopeMatches(snapshot, kGatewayBase + 1U, 1U, 1U));

  // A v3 staged page with no activated header is NOT a new authority.
  memset(new_header + 28U, 0xFF, 4U);
  assert(!c::decodePageHeader(new_header, kTracker, next_generation));
}
} // namespace

int main() {
  testCodecFailClosed();
  testGrantScopeAndCounters();
  testPowerCutsAndBootReplay();
  testMigrationSnapshotBoundary();
  puts("SF5K3C v3 custody-policy journal candidate: codec/scope/reboot/cuts PASS");
}
