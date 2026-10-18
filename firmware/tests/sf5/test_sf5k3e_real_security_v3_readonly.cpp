// SF5K3E: REAL SecurityStore production code read-only v3 flash probe.
// SF5K3I adds a separate live v3 Store owner; this diagnostic alone
// never authenticates Gateway RF ACK or releases custody.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>

#include "security_store.h"
#include "sf5k3c_security_v3_candidate.h"
#include "sf5k3d_security_v3_snapshot_candidate.h"
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

struct FakeFlash : FlashBackend {
  std::array<uint8_t, 8192> data{};
  bool fail_read = false;
  unsigned program_calls = 0U;
  unsigned erase_calls = 0U;
  FakeFlash() { data.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t length) const override {
    if (fail_read || !out || offset > data.size() ||
        length > data.size() - offset) return false;
    memcpy(out, data.data() + offset, length);
    return true;
  }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++program_calls;
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erase_calls;
    return FlashOpResult::kFailed;
  }
};

c::State fixtureState() {
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
    grant.value = kGateway + i + 1U;
    grant.grant_generation = 2U + i;
    grant.custody_floor = 0U;
    grant.policy_revision = i + 1U;
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
  c::Entry floor{};
  memcpy(floor.credential_id, state.credential_id, 16U);
  floor.key_epoch = state.key_epoch;
  floor.kind = c::Kind::kCustodyPolicy;
  floor.slot = 0xFFU;
  floor.value = 0U;
  floor.grant_generation = 0U;
  floor.custody_floor = 2U;
  floor.policy_revision = 6U;
  assert(c::applyHostAuthenticated(floor, true, state));
  return state;
}

void makeV3(FakeFlash& flash, const c::State& state) {
  uint8_t key[32]{};
  for (size_t i = 0U; i < sizeof(key); ++i)
    key[i] = static_cast<uint8_t>(0xA0U + i);
  uint8_t image[c::kPageSize]{};
  assert(d::makeV3SnapshotPage(kTracker, 3U, state, key, image));
  memcpy(flash.data.data(), image, sizeof(image));
}

SecurityV3ReadOnlySummary sentinel() {
  SecurityV3ReadOnlySummary s{};
  s.page_generation = 0xABCDEFU;
  s.key_epoch = 0x5555U;
  s.custody_policy_floor = 0x6666U;
  s.active_gateway_count = 99U;
  return s;
}

void assertUnchanged(const SecurityV3ReadOnlySummary& s) {
  assert(s.page_generation == 0xABCDEFU &&
         s.key_epoch == 0x5555U &&
         s.custody_policy_floor == 0x6666U &&
         s.active_gateway_count == 99U);
}

void testVerifiedButNeverAuthorized() {
  FakeFlash flash;
  const c::State fixture = fixtureState();
  makeV3(flash, fixture);
  SecurityStore real(flash, flash);
  SecurityV3ReadOnlySummary before_begin = sentinel();
  assert(real.inspectV3PageReadOnly(0U, before_begin) ==
         SecurityV3Inspection::kReadFault);
  assertUnchanged(before_begin);
  assert(real.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  // SF5K3I now recovers validated v3 credential/TX/A2D state in the
  // actual SecurityStore. This diagnostic still grants NO Gateway ACK
  // admission and never mutates flash. Automatic TX re-reservation is
  // only staged until poll(), so FakeFlash has no physical write here.
  assert(real.state() == SecurityState::kProvisioned);
  assert(real.busy());
  uint64_t tx = 0U;
  uint32_t epoch = 0U;
  assert(!real.reserveNextTxCounter(tx, epoch));
  bool accepted = true;
  assert(!real.takeA2dReplayResult(accepted));
  uint8_t id[sf::kCredentialIdSize]{};
  assert(real.currentCredentialId(id));
  assert(memcmp(id, fixture.credential_id, sizeof(id)) == 0);
  SecurityV3ReadOnlySummary summary = sentinel();
  assert(real.inspectV3PageReadOnly(0U, summary) ==
         SecurityV3Inspection::kVerified);
  assert(summary.page_generation == 3U && summary.key_epoch == 7U);
  assert(summary.tx_reserved_bound == 768U &&
         summary.a2d_replay_bound == 24U);
  assert(summary.custody_policy_floor == 2U &&
         summary.policy_revision == 6U);
  assert(summary.active_gateway_count == 3U &&
         summary.max_grant_generation == 6U);
  assert(real.state() == SecurityState::kProvisioned);
  assert(!real.reserveNextTxCounter(tx, epoch));
  // inspectV3PageReadOnly must never call program/erase. No poll is
  // called here because poll() is the separate live counter writer.
  assert(flash.program_calls == 0U && flash.erase_calls == 0U);
  assert(real.inspectV3PageReadOnly(1U, summary) ==
         SecurityV3Inspection::kNotV3);
  // No snapshot should overwrite output when a page is not v3.
  SecurityV3ReadOnlySummary untouched = sentinel();
  assert(real.inspectV3PageReadOnly(1U, untouched) ==
         SecurityV3Inspection::kNotV3);
  assertUnchanged(untouched);
  assert(real.inspectV3PageReadOnly(2U, untouched) ==
         SecurityV3Inspection::kReadFault);
  assertUnchanged(untouched);
  flash.fail_read = true;
  assert(real.inspectV3PageReadOnly(0U, untouched) ==
         SecurityV3Inspection::kReadFault);
  assertUnchanged(untouched);
}

void testStagedForeignAndCorruptAreNotPublished() {
  FakeFlash flash;
  makeV3(flash, fixtureState());
  SecurityStore real(flash, flash);
  assert(real.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  SecurityV3ReadOnlySummary output = sentinel();

  flash.data[28U] = 0xFFU;
  flash.data[29U] = 0xFFU;
  flash.data[30U] = 0xFFU;
  flash.data[31U] = 0xFFU;
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kStaged);
  assertUnchanged(output);

  makeV3(flash, fixtureState());
  flash.data[16U] ^= 0x01U;
  // Mutated committed header with bad CRC is corrupt, not FOREIGN.
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  makeV3(flash, fixtureState());
  uint8_t header[c::kHeaderSize]{};
  assert(c::encodePageHeader(3U, kTracker + 1U, header));
  memcpy(flash.data.data(), header, sizeof(header));
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kForeign);
  assertUnchanged(output);

  makeV3(flash, fixtureState());
  const uint32_t off = static_cast<uint32_t>(d::kJournalOffset);
  // Every mandatory prefix record has to be committed, CRC-valid and
  // consistent. A committed header with a torn prefix fails closed.
  for (size_t i = 0U; i < d::kSnapshotRecords; ++i) {
    FakeFlash damaged = flash;
    damaged.data[off + i * c::kEntrySize + 52U] = 0xFFU;
    SecurityStore checked(damaged, damaged);
    assert(checked.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
    assert(checked.inspectV3PageReadOnly(0U, output) ==
           SecurityV3Inspection::kCorrupt);
    assertUnchanged(output);
  }

  flash.data[off + 2U * c::kEntrySize + 32U] ^= 0x01U;
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  makeV3(flash, fixtureState());
  flash.data[c::kPageSize - 1U] = 0x00U; // reserved v3 tail
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  // Even a correctly CRC-sealed V3 credential is invalid when its scope
  // identity is all-zero/erased or key epoch is the reserved sentinel.
  makeV3(flash, fixtureState());
  sf::Credential credential{};
  assert(sf::decodeCredential(
      flash.data.data() + sf::kPageHeaderSize, credential));
  memset(credential.credential_id, 0U, sizeof(credential.credential_id));
  sf::encodeCredential(
      credential, flash.data.data() + sf::kPageHeaderSize);
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  makeV3(flash, fixtureState());
  assert(sf::decodeCredential(
      flash.data.data() + sf::kPageHeaderSize, credential));
  credential.key_epoch = UINT32_MAX;
  sf::encodeCredential(
      credential, flash.data.data() + sf::kPageHeaderSize);
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);
}

void testMonotonicTrailingPolicyAndCounter() {
  FakeFlash flash;
  const c::State state = fixtureState();
  makeV3(flash, state);
  SecurityStore real(flash, flash);
  assert(real.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  SecurityV3ReadOnlySummary output = sentinel();
  const uint32_t off = static_cast<uint32_t>(
      d::kJournalOffset + 7U * c::kEntrySize);
  c::Entry valid{};
  memcpy(valid.credential_id, state.credential_id, 16U);
  valid.key_epoch = state.key_epoch;
  valid.kind = c::Kind::kCustodyPolicy;
  valid.slot = 1U;
  valid.value = kGateway + 9U;
  valid.grant_generation = 7U;
  valid.custody_floor = 2U;
  valid.policy_revision = 7U;
  uint8_t encoded[c::kEntrySize]{};
  assert(c::encodeEntry(valid, encoded));
  memcpy(flash.data.data() + off, encoded, sizeof(encoded));
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kVerified);
  assert(output.active_gateway_count == 4U &&
         output.policy_revision == 7U &&
         output.max_grant_generation == 7U);

  // Authenticated-looking but stale revision must never be accepted;
  // CRC alone does not prove a valid policy transition.
  valid.policy_revision = 6U;
  assert(c::encodeEntry(valid, encoded));
  memcpy(flash.data.data() + off, encoded, sizeof(encoded));
  output = sentinel();
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  valid.policy_revision = 7U;
  valid.grant_generation = 6U; // reuse past generation
  assert(c::encodeEntry(valid, encoded));
  memcpy(flash.data.data() + off, encoded, sizeof(encoded));
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);

  // Erased journal hole followed by a later committed record is invalid.
  makeV3(flash, state);
  memcpy(flash.data.data() + off + c::kEntrySize, encoded,
         sizeof(encoded));
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kCorrupt);
  assertUnchanged(output);
}

void testV2StillRecognizedAsNotV3() {
  FakeFlash flash;
  const c::State state = fixtureState();
  uint8_t header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(sf::PageHeader{4U, kTracker},
                              sf::kVersionV2, header);
  memcpy(flash.data.data(), header, sizeof(header));
  sf::Credential c{};
  memcpy(c.credential_id, state.credential_id, 16U);
  c.key_epoch = state.key_epoch;
  c.device_identity = kTracker;
  memset(c.k_root, 0xA1, sizeof(c.k_root));
  sf::encodeCredential(c, flash.data.data() + sf::kPageHeaderSize);
  SecurityStore real(flash, flash);
  assert(real.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  assert(real.state() == SecurityState::kProvisioned);
  SecurityV3ReadOnlySummary output = sentinel();
  assert(real.inspectV3PageReadOnly(0U, output) ==
         SecurityV3Inspection::kNotV3);
  assertUnchanged(output);
  assert(real.state() == SecurityState::kProvisioned);
  // No physical writes from inspection despite real v2 service available.
  assert(flash.erase_calls == 0U && flash.program_calls == 0U);
}
}  // namespace

int main() {
  testVerifiedButNeverAuthorized();
  testStagedForeignAndCorruptAreNotPublished();
  testMonotonicTrailingPolicyAndCounter();
  testV2StillRecognizedAsNotV3();
  puts("SF5K3E SecurityStore v3 read-only diagnostic integrity: PASS");
}
