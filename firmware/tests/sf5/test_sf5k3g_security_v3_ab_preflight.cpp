// SF5K3G: actual FlashBackend A/B preflight and no-authority evidence.
// This is NOT production SecurityStore activation or a real flash writer.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>

#include "security_v3_ab_preflight.h"
#include "security_format.h"
#include "sf5k3c_security_v3_candidate.h"
#include "sf5k3d_security_v3_snapshot_candidate.h"
#include "journal_format.h"

using namespace orun_tlp;
namespace ab = security_v3_ab_preflight;
namespace sf = security_format;
namespace c = sf5k3c_candidate;
namespace d = sf5k3d_candidate;
namespace jf = journal_format;

namespace {
constexpr uint64_t kDevice = UINT64_C(0x0102030405060708);
constexpr uint64_t kGateway = UINT64_C(0x2222222222222200);

struct FakeFlash : public FlashBackend {
  std::array<uint8_t, 8192> data{};
  unsigned writes = 0U, erases = 0U;
  mutable int fail_at_read = -1;
  mutable unsigned reads = 0U;
  FakeFlash() { data.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t size) const override {
    ++reads;
    if ((fail_at_read > 0 &&
         static_cast<unsigned>(fail_at_read) == reads) ||
        !out || offset > data.size() ||
        size > data.size() - offset) return false;
    memcpy(out, data.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++writes;
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erases;
    return FlashOpResult::kFailed;
  }
  void write(unsigned page, const uint8_t (&raw)[c::kPageSize]) {
    assert(page < 2U);
    memcpy(data.data() + page * c::kPageSize, raw, sizeof(raw));
  }
  void reset() { data.fill(0xFFU); reads = 0U; fail_at_read = -1; }
};

void fillCredential(sf::Credential& credential, uint8_t seed,
                    uint64_t tracker = kDevice) {
  for (size_t i = 0U; i < sf::kCredentialIdSize; ++i)
    credential.credential_id[i] = static_cast<uint8_t>(seed + i);
  for (size_t i = 0U; i < sf::kKRootSize; ++i)
    credential.k_root[i] = static_cast<uint8_t>(0xA0U + seed + i);
  credential.key_epoch = 7U;
  credential.device_identity = tracker;
}

c::State stateWithPolicy(uint8_t seed = 1U) {
  c::State s{};
  sf::Credential cred{};
  fillCredential(cred, seed);
  memcpy(s.credential_id, cred.credential_id, 16U);
  s.key_epoch = cred.key_epoch;
  s.tx_bound = 768U;
  s.a2d_bound = 24U;
  for (uint8_t i = 0U; i < 4U; ++i) {
    c::Entry record{};
    memcpy(record.credential_id, s.credential_id, 16U);
    record.key_epoch = s.key_epoch;
    record.kind = c::Kind::kCustodyPolicy;
    record.slot = i;
    record.value = kGateway + i + 1U;
    record.grant_generation = 2U + i;
    record.policy_revision = i + 1U;
    assert(c::applyHostAuthenticated(record, true, s));
  }
  c::Entry revoke{};
  memcpy(revoke.credential_id, s.credential_id, 16U);
  revoke.key_epoch = s.key_epoch;
  revoke.kind = c::Kind::kCustodyPolicy;
  revoke.slot = 1U;
  revoke.value = 0U;
  revoke.grant_generation = 6U;
  revoke.policy_revision = 5U;
  assert(c::applyHostAuthenticated(revoke, true, s));
  c::Entry floor = revoke;
  floor.slot = 0xFFU;
  floor.grant_generation = 0U;
  floor.custody_floor = 2U;
  floor.policy_revision = 6U;
  assert(c::applyHostAuthenticated(floor, true, s));
  return s;
}

void makeV3(const c::State& state, uint64_t generation,
            uint8_t (&page)[c::kPageSize], uint8_t seed = 1U,
            uint64_t tracker = kDevice) {
  sf::Credential credential{};
  fillCredential(credential, seed, tracker);
  uint8_t root[sf::kKRootSize]{};
  memcpy(root, credential.k_root, sizeof(root));
  assert(d::makeV3SnapshotPage(tracker, generation, state, root, page));
}

void makeLegacy(uint8_t version, uint64_t generation,
                uint8_t (&page)[c::kPageSize],
                uint64_t tx_bound, uint64_t a2d_bound,
                uint8_t seed = 1U) {
  memset(page, 0xFF, sizeof(page));
  uint8_t header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(sf::PageHeader{generation, kDevice},
                              version, header);
  memcpy(page, header, sizeof(header));
  sf::Credential credential{};
  fillCredential(credential, seed);
  sf::encodeCredential(credential, page + sf::kPageHeaderSize);
  uint8_t id[sf::kCredentialIdSize]{};
  memcpy(id, credential.credential_id, sizeof(id));
  if (tx_bound != 0U) {
    if (version == sf::kVersionV1) {
      sf::TxReserve reserve{};
      memcpy(reserve.credential_id, id, sizeof(id));
      reserve.key_epoch = credential.key_epoch;
      reserve.tx_reserved_bound = tx_bound;
      uint8_t b[sf::kTxReserveRecordSize]{};
      sf::encodeTxReserve(reserve, b);
      memcpy(page + sf::kPageHeaderSize + sf::kCredentialRecordSize,
             b, sizeof(b));
    } else {
      sf::SecurityStateRecord state{};
      memcpy(state.credential_id, id, sizeof(id));
      state.key_epoch = credential.key_epoch;
      state.kind = sf::SecurityStateKind::kTxReserveExclusiveBound;
      state.value = tx_bound;
      uint8_t b[sf::kSecurityStateRecordSize]{};
      sf::encodeSecurityState(state, b);
      memcpy(page + sf::kPageHeaderSize + sf::kCredentialRecordSize,
             b, sizeof(b));
    }
  }
  if (a2d_bound != 0U) {
    assert(version == sf::kVersionV2);
    sf::SecurityStateRecord state{};
    memcpy(state.credential_id, id, sizeof(id));
    state.key_epoch = credential.key_epoch;
    state.kind = sf::SecurityStateKind::kA2dReplayExclusiveBound;
    state.value = a2d_bound;
    uint8_t b[sf::kSecurityStateRecordSize]{};
    sf::encodeSecurityState(state, b);
    const size_t slot = tx_bound != 0U ? 1U : 0U;
    memcpy(page + sf::kPageHeaderSize + sf::kCredentialRecordSize +
               slot * sf::kSecurityStateRecordSize,
           b, sizeof(b));
  }
}

ab::Summary sentinel() {
  ab::Summary s{};
  s.selected_page = 19U;
  s.generation = 99999U;
  s.policy_revision = 77U;
  s.tx_reserved_bound = 777U;
  return s;
}

void untouched(const ab::Summary& s) {
  assert(s.selected_page == 19U && s.generation == 99999U &&
         s.policy_revision == 77U && s.tx_reserved_bound == 777U);
}

void mustReturn(FakeFlash& flash, ab::Result expected) {
  ab::Summary out = sentinel();
  assert(ab::inspect(flash, kDevice, out) == expected);
  untouched(out);
  assert(flash.writes == 0U && flash.erases == 0U);
}

void testBlankStagedUnsupportedAndFault() {
  FakeFlash flash;
  mustReturn(flash, ab::Result::kUnprovisioned);

  // A seemingly erased page-header with a retained credential/journal body
  // can be an interrupted ERASE of an authoritative page. It is NOT empty.
  flash.data[100U] = 0U;
  mustReturn(flash, ab::Result::kFault);
  flash.reset();

  uint8_t page[c::kPageSize]{};
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.data[28U] = 0xFFU;
  flash.data[29U] = 0xFFU;
  flash.data[30U] = 0xFFU;
  flash.data[31U] = 0xFFU;
  mustReturn(flash, ab::Result::kUnprovisioned);

  flash.reset();
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.data[4U] = 4U;
  mustReturn(flash, ab::Result::kUnsupported);
  flash.data[28U] = 0xFFU;
  flash.data[29U] = 0xFFU;
  flash.data[30U] = 0xFFU;
  flash.data[31U] = 0xFFU;
  mustReturn(flash, ab::Result::kUnsupported); // future staged downgrade guard

  flash.reset();
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.data[24U] ^= 1U;
  mustReturn(flash, ab::Result::kFault);

  flash.reset();
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.data[100U + 4U*56U] ^= 1U;
  mustReturn(flash, ab::Result::kFault);
  flash.reset();
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.data[4095U] = 0U;
  mustReturn(flash, ab::Result::kFault);

  flash.reset();
  makeV3(stateWithPolicy(), 1U, page);
  flash.write(0U, page);
  flash.fail_at_read = 4;
  mustReturn(flash, ab::Result::kReadFault);
  flash.fail_at_read = -1;
}

void testLegacyAndV3Migration() {
  FakeFlash flash;
  uint8_t old_page[c::kPageSize]{};
  uint8_t new_page[c::kPageSize]{};
  const c::State state = stateWithPolicy();

  makeLegacy(sf::kVersionV1, 1U, old_page, 512U, 0U);
  flash.write(0U, old_page);
  ab::Summary out{};
  assert(ab::inspect(flash, kDevice, out) == ab::Result::kV1);
  assert(out.tx_reserved_bound == 512U && out.selected_version == 1U);
  assert(flash.writes == 0U && flash.erases == 0U);

  makeLegacy(sf::kVersionV2, 2U, new_page, 512U, 16U);
  flash.write(1U, new_page);
  assert(ab::inspect(flash, kDevice, out) == ab::Result::kV2);
  assert(out.selected_page == 1U && out.a2d_replay_bound == 16U);

  flash.reset();
  makeLegacy(sf::kVersionV2, 3U, old_page, 512U, 16U);
  flash.write(0U, old_page);
  makeV3(state, 4U, new_page);
  flash.write(1U, new_page);
  assert(ab::inspect(flash, kDevice, out) == ab::Result::kV3);
  assert(out.selected_version == 3U && out.selected_page == 1U &&
         out.generation == 4U);
  assert(out.tx_reserved_bound == 768U &&
         out.a2d_replay_bound == 24U);
  assert(out.custody_floor == 2U && out.policy_revision == 6U &&
         out.active_gateway_count == 3U &&
         out.max_grant_generation == 6U);

  // New V3 page cannot reuse a smaller counter value for same credential.
  c::State rollback = state;
  rollback.tx_bound = 256U;
  makeV3(rollback, 4U, new_page);
  flash.write(1U, new_page);
  mustReturn(flash, ab::Result::kFault);
  // A v3->v2 header regression must never lose known custody state.
  flash.reset();
  makeV3(state, 4U, old_page);
  flash.write(0U, old_page);
  makeLegacy(sf::kVersionV2, 5U, new_page, 1024U, 32U);
  flash.write(1U, new_page);
  mustReturn(flash, ab::Result::kFault);
}

void testV3CompactionAndSlotRollback() {
  FakeFlash flash;
  c::State state = stateWithPolicy();
  uint8_t page[c::kPageSize]{};
  makeV3(state, 5U, page);
  flash.write(0U, page);

  state.tx_bound = 1024U;
  state.a2d_bound = 32U;
  makeV3(state, 6U, page);
  flash.write(1U, page);
  ab::Summary out{};
  assert(ab::inspect(flash, kDevice, out) == ab::Result::kV3);
  assert(out.tx_reserved_bound == 1024U && out.a2d_replay_bound == 32U);
  assert(out.generation == 6U && out.policy_revision == 6U);
  assert(flash.writes == 0U && flash.erases == 0U);

  // More recent global generation hides dropped tombstone in slot 1.
  // The independent SF5K3D host model compared only global high water.
  // Product preflight additionally forbids PER-SLOT rollback.
  state.policy_revision = 7U;
  state.gateways[0].generation = 7U;
  state.gateways[1].generation = 0U; // revoke tombstone reverted!
  makeV3(state, 6U, page);
  flash.write(1U, page);
  mustReturn(flash, ab::Result::kFault);

  // Changing a Gateway identity but keeping its generation is also
  // impossible for a validated scoped grant/revoke transition.
  state = stateWithPolicy();
  state.policy_revision = 7U;
  state.gateways[0].gateway_id = kGateway + 99U;
  makeV3(state, 6U, page);
  flash.write(1U, page);
  mustReturn(flash, ab::Result::kFault);

  // Ambiguous equal committed page generations never select by index.
  state = stateWithPolicy();
  makeV3(state, 5U, page);
  flash.write(1U, page);
  mustReturn(flash, ab::Result::kFault);
}

void testRootRotationAndForeign() {
  FakeFlash flash;
  const c::State state = stateWithPolicy();
  uint8_t page[c::kPageSize]{};
  makeV3(state, 5U, page);
  flash.write(0U, page);

  c::State fresh{};
  sf::Credential new_credential{};
  fillCredential(new_credential, 9U);
  memcpy(fresh.credential_id, new_credential.credential_id, 16U);
  fresh.key_epoch = 7U; // equality allowed when both ID and root rotate
  makeV3(fresh, 6U, page, 9U);
  flash.write(1U, page);
  ab::Summary out{};
  assert(ab::inspect(flash, kDevice, out) == ab::Result::kV3);
  assert(out.policy_revision == 0U && out.active_gateway_count == 0U &&
         out.tx_reserved_bound == 0U && out.a2d_replay_bound == 0U);

  // Same ID but different key material may never masquerade as compaction.
  makeV3(state, 6U, page, 9U);
  flash.write(1U, page);
  mustReturn(flash, ab::Result::kFault);

  // New ID while keeping old key/root must also fail.
  makeV3(fresh, 6U, page, 9U);
  sf::Credential cred{};
  assert(sf::decodeCredential(page + sf::kPageHeaderSize, cred));
  sf::Credential old_cred{};
  fillCredential(old_cred, 1U);
  memcpy(cred.k_root, old_cred.k_root, sizeof(cred.k_root));
  sf::encodeCredential(cred, page + sf::kPageHeaderSize);
  flash.write(1U, page);
  mustReturn(flash, ab::Result::kFault);

  flash.reset();
  makeV3(state, 5U, page);
  flash.write(0U, page);
  uint8_t header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(
      sf::PageHeader{5U, kDevice + 1U}, 3U, header);
  memcpy(flash.data.data(), header, sizeof(header));
  mustReturn(flash, ab::Result::kFault); // credential/header binding fails

  flash.reset();
  makeV3(state, 5U, page, 1U, kDevice + 1U);
  flash.write(0U, page);
  mustReturn(flash, ab::Result::kForeign);
}

void testCutAndNoPublication() {
  FakeFlash flash;
  c::State state = stateWithPolicy();
  uint8_t page[c::kPageSize]{};
  makeV3(state, 5U, page);
  flash.write(0U, page);
  const uint32_t record_at = 100U + 7U * 56U;
  c::Entry added{};
  memcpy(added.credential_id, state.credential_id, 16U);
  added.key_epoch = state.key_epoch;
  added.kind = c::Kind::kTxReserve;
  added.value = 1024U;
  uint8_t record[c::kEntrySize]{};
  assert(c::encodeEntry(added, record));
  ab::Summary out{};

  for (uint32_t cut = 0U; cut <= sizeof(record); ++cut) {
    memset(flash.data.data() + record_at, 0xFF, sizeof(record));
    memcpy(flash.data.data() + record_at, record, cut);
    out = sentinel();
    const ab::Result status = ab::inspect(flash, kDevice, out);
    if (cut <= 52U) {
      assert(status == ab::Result::kV3);
      assert(out.tx_reserved_bound == 768U);
    } else if (cut == sizeof(record)) {
      assert(status == ab::Result::kV3);
      assert(out.tx_reserved_bound == 1024U);
    } else {
      assert(status == ab::Result::kFault);
      untouched(out);
    }
  }
  assert(flash.writes == 0U && flash.erases == 0U);

  // Damaged older committed v3 page cannot be skipped in favour of a
  // larger-generation new page: selection must stop on corruption.
  flash.reset();
  makeV3(state, 4U, page);
  flash.write(0U, page);
  makeV3(state, 5U, page);
  flash.write(1U, page);
  flash.data[100U + 2U * 56U] ^= 1U;
  mustReturn(flash, ab::Result::kFault);

  // Verify known caller data remains untouched even if a later read fails.
  flash.reset();
  makeV3(state, 5U, page);
  flash.write(0U, page);
  flash.fail_at_read = 7;
  mustReturn(flash, ab::Result::kReadFault);
}
}  // namespace

int main() {
  testBlankStagedUnsupportedAndFault();
  testLegacyAndV3Migration();
  testV3CompactionAndSlotRollback();
  testRootRotationAndForeign();
  testCutAndNoPublication();
  puts("SF5K3G real FlashBackend v1/v2/v3 A/B read-only preflight: PASS");
}
