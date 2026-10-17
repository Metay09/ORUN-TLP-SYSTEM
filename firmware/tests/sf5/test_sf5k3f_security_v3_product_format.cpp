// SF5K3F: production-source v3 journal byte compatibility and cut tests.
// The real SecurityStore still writes only v2. No flash mutation here.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "security_v3_format.h"
#include "sf5k3c_security_v3_candidate.h"
#include "sf5k3d_security_v3_snapshot_candidate.h"
#include "security_format.h"
#include "journal_format.h"

using namespace orun_tlp;
namespace v3 = security_v3_format;
namespace c = sf5k3c_candidate;
namespace d = sf5k3d_candidate;
namespace jf = journal_format;

namespace {
constexpr uint64_t kTracker = UINT64_C(0x0102030405060708);
constexpr uint64_t kBase = UINT64_C(0x2122232425262700);

c::State referenceFixture() {
  c::State state{};
  for (size_t i = 0U; i < 16U; ++i)
    state.credential_id[i] = static_cast<uint8_t>(i + 1U);
  state.key_epoch = 7U;
  state.tx_bound = 768U;
  state.a2d_bound = 24U;
  for (uint8_t i = 0U; i < 4U; ++i) {
    c::Entry grant{};
    memcpy(grant.credential_id, state.credential_id, 16U);
    grant.key_epoch = state.key_epoch;
    grant.kind = c::Kind::kCustodyPolicy;
    grant.slot = i;
    grant.value = kBase + i + 1U;
    grant.grant_generation = static_cast<uint32_t>(i) + 2U;
    grant.custody_floor = 0U;
    grant.policy_revision = static_cast<uint32_t>(i) + 1U;
    assert(c::applyHostAuthenticated(grant, true, state));
  }
  c::Entry revoke{};
  memcpy(revoke.credential_id, state.credential_id, 16U);
  revoke.key_epoch = state.key_epoch;
  revoke.kind = c::Kind::kCustodyPolicy;
  revoke.slot = 1U;
  revoke.value = 0U;
  revoke.grant_generation = 6U;
  revoke.custody_floor = 0U;
  revoke.policy_revision = 5U;
  assert(c::applyHostAuthenticated(revoke, true, state));
  c::Entry floor = revoke;
  floor.slot = 0xFFU;
  floor.grant_generation = 0U;
  floor.custody_floor = 2U;
  floor.policy_revision = 6U;
  assert(c::applyHostAuthenticated(floor, true, state));
  return state;
}

v3::Snapshot toProduct(const c::State& ref) {
  v3::Snapshot s{};
  memcpy(s.credential_id, ref.credential_id, 16U);
  s.key_epoch = ref.key_epoch;
  s.tx_bound = ref.tx_bound;
  s.a2d_bound = ref.a2d_bound;
  s.custody_floor = ref.custody_floor;
  s.policy_revision = ref.policy_revision;
  for (size_t i = 0U; i < v3::kGatewayCount; ++i) {
    s.gateways[i].gateway_id = ref.gateways[i].gateway_id;
    s.gateways[i].generation = ref.gateways[i].generation;
  }
  return s;
}

void assertEqual(const v3::Snapshot& a, const v3::Snapshot& b) {
  assert(memcmp(a.credential_id, b.credential_id, 16U) == 0);
  assert(a.key_epoch == b.key_epoch &&
         a.tx_bound == b.tx_bound && a.a2d_bound == b.a2d_bound &&
         a.custody_floor == b.custody_floor &&
         a.policy_revision == b.policy_revision);
  for (size_t i = 0U; i < v3::kGatewayCount; ++i) {
    assert(a.gateways[i].gateway_id == b.gateways[i].gateway_id);
    assert(a.gateways[i].generation == b.gateways[i].generation);
  }
}

void fixturePage(const c::State& s, uint8_t (&page)[c::kPageSize]) {
  uint8_t root[32]{};
  for (size_t i = 0U; i < sizeof(root); ++i)
    root[i] = static_cast<uint8_t>(0xA0U + i);
  assert(d::makeV3SnapshotPage(kTracker, 5U, s, root, page));
}

void testByteIdenticalToCandidate() {
  static_assert(v3::kRecordBytes == c::kEntrySize &&
                v3::kRecordCount == c::kSlotCount &&
                v3::kTailBytes == c::kTailBytes &&
                v3::kJournalBytes == d::kEntryAreaSize,
                "production codec geometry must equal host candidate");
  static_assert(security_format::kVersion == security_format::kVersionV2,
                "no runtime v3 activation or migration");
  const c::State reference = referenceFixture();
  const v3::Snapshot snapshot = toProduct(reference);
  uint8_t product[v3::kSnapshotCount][v3::kRecordBytes]{};
  uint8_t candidate[d::kSnapshotRecords][c::kEntrySize]{};
  assert(v3::encodeSnapshotPrefix(snapshot, product));
  assert(d::encodeSnapshot(reference, candidate));
  assert(memcmp(product, candidate, sizeof(product)) == 0);

  const c::Entry oracle = [&reference]() {
    c::Entry e{};
    memcpy(e.credential_id, reference.credential_id, 16U);
    e.key_epoch = reference.key_epoch;
    e.kind = c::Kind::kCustodyPolicy;
    e.slot = 1U;
    e.value = kBase + 9U;
    e.grant_generation = 7U;
    e.custody_floor = reference.custody_floor;
    e.policy_revision = reference.policy_revision + 1U;
    return e;
  }();
  v3::Entry prod{};
  memcpy(prod.credential_id, oracle.credential_id, 16U);
  prod.key_epoch = oracle.key_epoch;
  prod.kind = v3::Kind::kCustodyPolicy;
  prod.slot = oracle.slot;
  prod.value = oracle.value;
  prod.grant_generation = oracle.grant_generation;
  prod.custody_floor = oracle.custody_floor;
  prod.policy_revision = oracle.policy_revision;
  uint8_t one[v3::kRecordBytes]{};
  uint8_t oracle_bytes[c::kEntrySize]{};
  assert(v3::encodeRecord(prod, one) &&
         c::encodeEntry(oracle, oracle_bytes));
  assert(memcmp(one, oracle_bytes, sizeof(one)) == 0);
  v3::Entry decoded{};
  assert(v3::decodeCommittedRecord(one, decoded));
  assert(decoded.value == prod.value &&
         decoded.grant_generation == prod.grant_generation);

  // Independently computed SF5K3C exact 56-byte golden is carried by
  // test_sf5k3c_security_v3_candidate.cpp. Here compare every snapshot
  // and post-snapshot byte against that separate implementation.
  uint8_t page[c::kPageSize]{};
  fixturePage(reference, page);
  assert(memcmp(page + v3::kJournalOffset,
                product, sizeof(product)) == 0);
  v3::Snapshot recovered{};
  assert(v3::recoverCommittedJournal(page + v3::kJournalOffset,
                                     v3::kJournalBytes,
                                     snapshot.credential_id,
                                     snapshot.key_epoch, recovered));
  assertEqual(recovered, snapshot);
}

void testCorruptionAndPublication() {
  const c::State reference = referenceFixture();
  const v3::Snapshot expected = toProduct(reference);
  uint8_t page[c::kPageSize]{};
  fixturePage(reference, page);
  v3::Snapshot untouched = expected;
  untouched.custody_floor = 99U;
  // Every byte in all mandatory 5 committed prefix entries is integrity
  // protected, including 0-valued commit and reserved fields.
  for (size_t offset = 0U;
       offset < v3::kSnapshotCount * v3::kRecordBytes; ++offset) {
    page[v3::kJournalOffset + offset] ^= 1U;
    v3::Snapshot output = untouched;
    assert(!v3::recoverCommittedJournal(
        page + v3::kJournalOffset, v3::kJournalBytes,
        expected.credential_id, expected.key_epoch, output));
    assertEqual(output, untouched);
    page[v3::kJournalOffset + offset] ^= 1U;
  }

  // Staged/incomplete snapshot cannot give anyone authority.
  const size_t cut = v3::kJournalOffset + 52U;
  page[cut] = 0xFFU;
  assert(!v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      expected.credential_id, expected.key_epoch, untouched));
  page[cut] = 0U;
  page[c::kPageSize - 1U] = 0U;
  assert(!v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      expected.credential_id, expected.key_epoch, untouched));

  // Valid CRC over semantically impossible duplicate Gateway state
  // must never bypass policy checks.
  fixturePage(reference, page);
  uint8_t changed[v3::kRecordBytes]{};
  memcpy(changed, page + v3::kJournalOffset + 2U*v3::kRecordBytes,
         sizeof(changed));
  v3::Entry slot{};
  assert(v3::decodeCommittedRecord(changed, slot));
  slot.value = kBase + 1U;  // duplicates active slot zero
  slot.grant_generation = 7U;
  assert(v3::encodeRecord(slot, changed));
  memcpy(page + v3::kJournalOffset + 2U*v3::kRecordBytes,
         changed, sizeof(changed));
  assert(!v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      expected.credential_id, expected.key_epoch, untouched));
  assert(untouched.custody_floor == 99U);
}

void testJournalTransitionsAndCuts() {
  c::State ref = referenceFixture();
  const v3::Snapshot snapshot = toProduct(ref);
  uint8_t page[c::kPageSize]{};
  fixturePage(ref, page);

  v3::Entry next{};
  memcpy(next.credential_id, snapshot.credential_id, 16U);
  next.key_epoch = snapshot.key_epoch;
  next.kind = v3::Kind::kCustodyPolicy;
  next.slot = 1U;
  next.value = kBase + 9U;
  next.grant_generation = 7U;
  next.custody_floor = 2U;
  next.policy_revision = 7U;
  uint8_t record[v3::kRecordBytes]{};
  assert(v3::encodeRecord(next, record));
  const size_t at = v3::kJournalOffset + 7U * v3::kRecordBytes;
  memcpy(page + at, record, sizeof(record));

  v3::Snapshot accepted{};
  assert(v3::recoverCommittedJournal(page + v3::kJournalOffset,
                                     v3::kJournalBytes,
                                     snapshot.credential_id,
                                     snapshot.key_epoch, accepted));
  assert(accepted.gateways[1].gateway_id == kBase + 9U);
  assert(accepted.gateways[1].generation == 7U);
  assert(accepted.policy_revision == 7U);
  assert(accepted.tx_bound == 768U && accepted.a2d_bound == 24U);

  // Exhaustively interrupt every byte of a late append: a torn body
  // can burn a slot, an ambiguous partial commit must fail closed.
  for (size_t cut = 0U; cut <= v3::kRecordBytes; ++cut) {
    memset(page + at, 0xFF, sizeof(record));
    memcpy(page + at, record, cut);
    v3::Snapshot output{};
    const bool ok = v3::recoverCommittedJournal(
        page + v3::kJournalOffset, v3::kJournalBytes,
        snapshot.credential_id, snapshot.key_epoch, output);
    if (cut <= 52U) {
      assert(ok);
      assertEqual(output, snapshot);
    } else if (cut == v3::kRecordBytes) {
      assert(ok);
      assert(output.policy_revision == 7U);
    } else {
      assert(!ok);
    }
  }

  // A policy snapshot injected into an ordinary journal tail is invalid
  // even if its bytes, CRC and commit are independently well-formed.
  fixturePage(ref, page);
  uint8_t prefixes[v3::kSnapshotCount][v3::kRecordBytes]{};
  assert(v3::encodeSnapshotPrefix(snapshot, prefixes));
  memcpy(page + at, prefixes[0], v3::kRecordBytes);
  v3::Snapshot unchanged = snapshot;
  assert(!v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      snapshot.credential_id, snapshot.key_epoch, unchanged));
  assertEqual(unchanged, snapshot);

  // Counter records are typed and monotone, not accepted as policy.
  fixturePage(ref, page);
  v3::Entry count{};
  memcpy(count.credential_id, snapshot.credential_id, 16U);
  count.key_epoch = snapshot.key_epoch;
  count.kind = v3::Kind::kTxReserve;
  count.value = 1024U;
  assert(v3::encodeRecord(count, record));
  memcpy(page + at, record, sizeof(record));
  assert(v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      snapshot.credential_id, snapshot.key_epoch, accepted));
  assert(accepted.tx_bound == 1024U &&
         accepted.a2d_bound == snapshot.a2d_bound);
  count.value = 512U;
  assert(v3::encodeRecord(count, record));
  memcpy(page + at, record, sizeof(record));
  assert(!v3::recoverCommittedJournal(
      page + v3::kJournalOffset, v3::kJournalBytes,
      snapshot.credential_id, snapshot.key_epoch, accepted));
}

void testInvalidEncodeNoPublication() {
  v3::Snapshot state = toProduct(referenceFixture());
  uint8_t prefix[v3::kSnapshotCount][v3::kRecordBytes];
  memset(prefix, 0xA5, sizeof(prefix));
  state.gateways[0].generation = UINT32_MAX;
  assert(!v3::encodeSnapshotPrefix(state, prefix));
  for (size_t i = 0U; i < sizeof(prefix); ++i)
    assert(reinterpret_cast<uint8_t*>(prefix)[i] == 0xA5U);

  const c::State ref = referenceFixture();
  v3::Entry bad{};
  memcpy(bad.credential_id, ref.credential_id, 16U);
  bad.key_epoch = ref.key_epoch;
  bad.kind = v3::Kind::kCustodyPolicy;
  bad.slot = 4U; // only 0..3
  bad.value = kBase + 5U;
  bad.grant_generation = 8U;
  bad.policy_revision = 7U;
  uint8_t bytes[v3::kRecordBytes];
  memset(bytes, 0xA5, sizeof(bytes));
  assert(!v3::encodeRecord(bad, bytes));
  for (size_t i = 0U; i < sizeof(bytes); ++i)
    assert(bytes[i] == 0xA5U);
}
}  // namespace

int main() {
  testByteIdenticalToCandidate();
  testCorruptionAndPublication();
  testJournalTransitionsAndCuts();
  testInvalidEncodeNoPublication();
  puts("SF5K3F production v3 format golden/snapshot/cuts: PASS");
}
