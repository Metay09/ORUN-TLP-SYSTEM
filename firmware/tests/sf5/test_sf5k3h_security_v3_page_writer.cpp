// SF5K3H physical FlashBackend v3 migration/compaction transaction tests.
// No production boot wiring. Each mutation is made through the SAME
// FlashBackend used by SecurityStore, but all actual hardware is mocked.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>

#include "security_format.h"
#include "security_v3_page_writer.h"
#include "security_v3_ab_preflight.h"
#include "sf5k3c_security_v3_candidate.h"
#include "sf5k3d_security_v3_snapshot_candidate.h"

using namespace orun_tlp;
namespace sf = security_format;
namespace v3 = security_v3_format;
namespace wr = security_v3_page_writer;
namespace ab = security_v3_ab_preflight;
namespace c = sf5k3c_candidate;
namespace d = sf5k3d_candidate;

namespace {
constexpr uint64_t kTracker = UINT64_C(0x0102030405060708);
constexpr uint64_t kGateway = UINT64_C(0x2200220000000000);
constexpr uint32_t kPageSize = 4096U;

struct Flash : public FlashBackend {
  std::array<uint8_t, 8192U> bytes{};
  int fault_call = -1;
  int corrupt_call = -1;
  uint32_t prefix = 0U;
  bool pending_every = false;
  bool late_failed_completion = false;
  int late_failure_call = -1;
  bool unreconciled = false;
  bool pending = false;
  bool pending_erase = false;
  uint32_t pending_offset = 0U;
  uint32_t pending_size = 0U;
  uint8_t pending_bytes[68U]{};
  uint32_t pending_page = 0U;
  uint32_t accepted_calls = 0U;
  uint32_t programmed = 0U, erased = 0U, poll_calls = 0U;

  Flash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* dst, size_t n) const override {
    if (!dst || offset > bytes.size() || n > bytes.size() - offset)
      return false;
    memcpy(dst, bytes.data() + offset, n);
    return true;
  }
  void applyProgram(uint32_t offset, const uint8_t* source,
                    uint32_t n) {
    for (uint32_t i = 0U; i < n; ++i) bytes[offset+i] &= source[i];
    ++programmed;
  }
  FlashOpResult program(uint32_t offset, const void* raw,
                        size_t count) override {
    assert(!pending);
    ++accepted_calls;
    if (!raw || count == 0U || count > sizeof(pending_bytes) ||
        (offset % 4U) != 0U || (count % 4U) != 0U ||
        offset > bytes.size() || count > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const uint8_t* data = static_cast<const uint8_t*>(raw);
    for (size_t i = 0U; i < count; ++i)
      if (bytes[offset+i] != 0xFFU) return FlashOpResult::kFailed;
    if (static_cast<int>(accepted_calls) == fault_call) {
      for (uint32_t i = 0U; i < prefix && i < count; ++i)
        bytes[offset+i] &= data[i];
      return FlashOpResult::kFailed;
    }
    if (pending_every) {
      pending = true;
      pending_erase = false;
      pending_offset = offset;
      pending_size = static_cast<uint32_t>(count);
      memcpy(pending_bytes, data, count);
      return FlashOpResult::kPending;
    }
    applyProgram(offset, data, static_cast<uint32_t>(count));
    if (static_cast<int>(accepted_calls) == corrupt_call) {
      // Additional cleared bit; successful backend status cannot
      // trick page writer's immediate readback check.
      for (size_t i = 0U; i < count; ++i) {
        if (data[i] != 0U) {
          bytes[offset+i] &= 0U;
          break;
        }
      }
    }
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    assert(!pending);
    ++accepted_calls;
    if (page >= 2U) return FlashOpResult::kFailed;
    if (static_cast<int>(accepted_calls) == fault_call) {
      memset(bytes.data() + page * kPageSize, 0xFFU,
             prefix < kPageSize ? prefix : kPageSize);
      return FlashOpResult::kFailed;
    }
    if (pending_every) {
      pending = true;
      pending_erase = true;
      pending_page = page;
      return FlashOpResult::kPending;
    }
    memset(bytes.data() + page * kPageSize, 0xFFU, kPageSize);
    ++erased;
    return FlashOpResult::kDone;
  }
  FlashOpResult pollPending() override {
    ++poll_calls;
    assert(pending);
    pending = false;
    if (pending_erase) {
      memset(bytes.data() + pending_page * kPageSize, 0xFFU, kPageSize);
      ++erased;
    } else {
      applyProgram(pending_offset, pending_bytes, pending_size);
    }
    const bool fail = late_failed_completion ||
                      (late_failure_call > 0 &&
                       accepted_calls ==
                           static_cast<uint32_t>(late_failure_call));
    return fail ? FlashOpResult::kFailed : FlashOpResult::kDone;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }
};

sf::Credential credential(uint8_t seed = 1U) {
  sf::Credential c{};
  c.device_identity = kTracker;
  c.key_epoch = 7U;
  for (unsigned i = 0U; i < sf::kCredentialIdSize; ++i)
    c.credential_id[i] = static_cast<uint8_t>(seed + i);
  for (unsigned i = 0U; i < sf::kKRootSize; ++i)
    c.k_root[i] = static_cast<uint8_t>(0xA0U + seed + i);
  return c;
}

void v2Page(Flash& flash, unsigned page, uint64_t generation,
            uint64_t tx, uint64_t a2d,
            const sf::Credential& cred = credential()) {
  assert(page < 2U);
  uint8_t* dest = flash.bytes.data() + page * kPageSize;
  memset(dest, 0xFFU, kPageSize);
  uint8_t header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(
      sf::PageHeader{generation, cred.device_identity},
      sf::kVersionV2, header);
  memcpy(dest, header, sizeof(header));
  sf::encodeCredential(cred, dest + sf::kPageHeaderSize);
  unsigned used = 0U;
  for (unsigned kind = 0U; kind < 2U; ++kind) {
    const uint64_t bound = kind == 0U ? tx : a2d;
    if (bound == 0U) continue;
    sf::SecurityStateRecord rec{};
    memcpy(rec.credential_id, cred.credential_id,
           sf::kCredentialIdSize);
    rec.key_epoch = cred.key_epoch;
    rec.kind = kind == 0U ?
        sf::SecurityStateKind::kTxReserveExclusiveBound :
        sf::SecurityStateKind::kA2dReplayExclusiveBound;
    rec.value = bound;
    uint8_t bytes[sf::kSecurityStateRecordSize]{};
    sf::encodeSecurityState(rec, bytes);
    memcpy(dest + sf::kPageHeaderSize +
               sf::kCredentialRecordSize +
               used * sf::kSecurityStateRecordSize,
           bytes, sizeof(bytes));
    ++used;
  }
}

v3::Snapshot emptySnapshot(uint64_t tx, uint64_t a2d,
                           const sf::Credential& cred = credential()) {
  v3::Snapshot s{};
  memcpy(s.credential_id, cred.credential_id,
         sf::kCredentialIdSize);
  s.key_epoch = cred.key_epoch;
  s.tx_bound = tx;
  s.a2d_bound = a2d;
  return s;
}

c::State withGrants() {
  const sf::Credential cred = credential();
  c::State s{};
  memcpy(s.credential_id, cred.credential_id,
         sf::kCredentialIdSize);
  s.key_epoch = cred.key_epoch;
  s.tx_bound = 768U;
  s.a2d_bound = 24U;
  for (uint8_t i = 0U; i < 3U; ++i) {
    c::Entry grant{};
    memcpy(grant.credential_id, s.credential_id,
           sf::kCredentialIdSize);
    grant.key_epoch = s.key_epoch;
    grant.kind = c::Kind::kCustodyPolicy;
    grant.slot = i;
    grant.value = kGateway + i + 1U;
    grant.grant_generation = 2U + i;
    grant.policy_revision = 1U + i;
    assert(c::applyHostAuthenticated(grant, true, s));
  }
  c::Entry revoke{};
  memcpy(revoke.credential_id, s.credential_id,
         sf::kCredentialIdSize);
  revoke.key_epoch = s.key_epoch;
  revoke.kind = c::Kind::kCustodyPolicy;
  revoke.slot = 1U;
  revoke.value = 0U;
  revoke.grant_generation = 5U;
  revoke.policy_revision = 4U;
  assert(c::applyHostAuthenticated(revoke, true, s));
  return s;
}

v3::Snapshot fromRef(const c::State& state) {
  v3::Snapshot out{};
  memcpy(out.credential_id, state.credential_id,
         sf::kCredentialIdSize);
  out.key_epoch = state.key_epoch;
  out.tx_bound = state.tx_bound;
  out.a2d_bound = state.a2d_bound;
  out.custody_floor = state.custody_floor;
  out.policy_revision = state.policy_revision;
  for (unsigned i = 0U; i < v3::kGatewayCount; ++i) {
    out.gateways[i].gateway_id = state.gateways[i].gateway_id;
    out.gateways[i].generation = state.gateways[i].generation;
  }
  return out;
}

void seedV3(Flash& flash, unsigned page, uint64_t generation,
            const c::State& state) {
  uint8_t img[c::kPageSize]{};
  const sf::Credential cred = credential();
  assert(d::makeV3SnapshotPage(kTracker, generation,
                              state, cred.k_root, img));
  memcpy(flash.bytes.data() + page * kPageSize, img, sizeof(img));
}

void finish(wr::SnapshotTransaction& writer, unsigned max = 200U) {
  for (unsigned i = 0U;
       i < max && writer.status() == wr::Status::kBusy; ++i)
    writer.poll();
  assert(writer.status() != wr::Status::kBusy);
}

void verifyPrefix(Flash& flash, unsigned page,
                  uint64_t expect_generation) {
  ab::Summary summary{};
  assert(ab::inspect(flash, kTracker, summary) == ab::Result::kV3);
  assert(summary.selected_page == page);
  assert(summary.generation == expect_generation);
}

void testV2MigrationThenV3Compaction() {
  Flash flash;
  v2Page(flash, 0U, 6U, 768U, 24U);
  const auto src = flash.bytes;
  const v3::Snapshot snapshot = emptySnapshot(768U, 24U);
  wr::SnapshotTransaction tx(flash);
  assert(tx.start(1U, credential(), snapshot));
  finish(tx);
  assert(tx.status() == wr::Status::kCommitted);
  assert(memcmp(src.data(), flash.bytes.data(), kPageSize) == 0);
  verifyPrefix(flash, 1U, 7U);
  assert(tx.physicalMutationCompletions() == 19U);
  assert(!tx.start(0U, credential(), snapshot));

  // Recovered v3 is the only eligible physical source for a second
  // separately owned compaction into the other A/B page.
  wr::SnapshotTransaction compact(flash);
  assert(compact.start(0U, credential(), snapshot));
  finish(compact);
  assert(compact.status() == wr::Status::kCommitted);
  verifyPrefix(flash, 0U, 8U);
}

void testGatewaysAndRefusal() {
  Flash flash;
  const c::State source = withGrants();
  seedV3(flash, 0U, 4U, source);
  const v3::Snapshot carried = fromRef(source);

  v3::Snapshot forbidden = carried;
  forbidden.gateways[1].generation = 0U; // drops revoke tombstone
  wr::SnapshotTransaction tx(flash);
  assert(!tx.start(1U, credential(), forbidden));
  assert(flash.accepted_calls == 0U);
  forbidden = carried;
  forbidden.gateways[0].gateway_id += 100U;
  assert(!tx.start(1U, credential(), forbidden));
  forbidden = carried;
  forbidden.tx_bound = 1024U; // no auth counter owner in writer
  assert(!tx.start(1U, credential(), forbidden));
  const sf::Credential wrong = credential(9U);
  assert(!tx.start(1U, wrong, carried));
  assert(flash.accepted_calls == 0U);

  assert(tx.start(1U, credential(), carried));
  finish(tx);
  assert(tx.status() == wr::Status::kCommitted);
  verifyPrefix(flash, 1U, 5U);
  ab::Summary s{};
  assert(ab::inspect(flash, kTracker, s) == ab::Result::kV3);
  assert(s.policy_revision == source.policy_revision &&
         s.max_grant_generation == 5U &&
         s.active_gateway_count == 2U);
}

void testByteCutsAndSourcePreservation() {
  // 18 physical mutations: 1 erase + 1 header body + 2 credential
  // + 14 journal phases (5 snapshot + 2 bounds) + 1 activation.
  for (int call = 1; call <= 19; ++call) {
    Flash flash;
    v2Page(flash, 0U, 6U, 768U, 24U);
    const auto old = flash.bytes;
    flash.fault_call = call;
    flash.prefix = static_cast<uint32_t>(call) % 5U;
    wr::SnapshotTransaction tx(flash);
    assert(tx.start(1U, credential(), emptySnapshot(768U,24U)));
    finish(tx);
    assert(tx.status() == wr::Status::kFault);
    assert(memcmp(flash.bytes.data(), old.data(), kPageSize) == 0);
    assert(tx.poll() == wr::Status::kFault);
    assert(flash.accepted_calls == static_cast<uint32_t>(call));
    ab::Summary s{};
    const ab::Result recovered = ab::inspect(flash, kTracker, s);
    // The final 4-byte activation write can physically land despite
    // kFailed (the API intentionally treats that as ambiguous). Never
    // claim the old page remained authoritative in that final cut.
    assert(recovered == ab::Result::kV2 ||
           recovered == ab::Result::kFault ||
           (call == 19 && recovered == ab::Result::kV3));
  }
}

void testPendingAndLateCompletion() {
  Flash flash;
  v2Page(flash, 0U, 6U, 768U, 24U);
  flash.pending_every = true;
  wr::SnapshotTransaction tx(flash);
  assert(tx.start(1U, credential(), emptySnapshot(768U, 24U)));
  finish(tx);
  assert(tx.status() == wr::Status::kCommitted);
  assert(flash.accepted_calls == 19U);
  assert(flash.poll_calls == 19U);
  assert(flash.programmed == 18U && flash.erased == 1U);
  verifyPrefix(flash, 1U, 7U);

  Flash ambiguous;
  v2Page(ambiguous, 0U, 6U, 768U, 24U);
  ambiguous.pending_every = true;
  ambiguous.late_failed_completion = true;
  wr::SnapshotTransaction maybe(ambiguous);
  assert(maybe.start(1U, credential(), emptySnapshot(768U,24U)));
  finish(maybe);
  assert(maybe.status() == wr::Status::kFault);
  assert(maybe.poll() == wr::Status::kFault);
  assert(ambiguous.accepted_calls == 1U);
  assert(ambiguous.poll_calls == 1U);
}

void testAmbiguousAcceptedFinalActivation() {
  // SoftDevice may have physically persisted the activation commit even
  // when the completion callback arrives as failure. The transaction
  // becomes FAULT (never returns an authorization success), while the
  // independently recovered physical page may be valid v3 on reboot.
  Flash flash;
  v2Page(flash, 0U, 6U, 768U, 24U);
  flash.pending_every = true;
  flash.late_failure_call = 19;
  wr::SnapshotTransaction tx(flash);
  assert(tx.start(1U, credential(), emptySnapshot(768U,24U)));
  finish(tx);
  assert(tx.status() == wr::Status::kFault);
  assert(tx.poll() == wr::Status::kFault);
  assert(flash.accepted_calls == 19U);
  assert(flash.poll_calls == 19U);
  ab::Summary recovered{};
  assert(ab::inspect(flash, kTracker, recovered) == ab::Result::kV3);
  assert(recovered.generation == 7U &&
         recovered.selected_page == 1U);
}

void testTamperPreactivationAndDirtyReadback() {
  Flash flash;
  v2Page(flash, 0U, 6U, 768U, 24U);
  wr::SnapshotTransaction tx(flash);
  assert(tx.start(1U, credential(), emptySnapshot(768U, 24U)));
  for (unsigned i = 0U;
       tx.status() == wr::Status::kBusy && i < 200U; ++i) {
    if (tx.physicalMutationCompletions() == 18U) {
      flash.bytes[4096U + 100U + 3U * 56U] ^= 0x01U;
      break;
    }
    tx.poll();
  }
  finish(tx);
  assert(tx.status() == wr::Status::kFault);
  assert(flash.accepted_calls == 18U);
  assert(flash.bytes[4096U + 28U] == 0xFFU);
  assert(flash.bytes[4096U + 29U] == 0xFFU);

  Flash bad_readback;
  v2Page(bad_readback, 0U, 6U, 768U, 24U);
  bad_readback.corrupt_call = 2; // header body programmed, but wrong bits
  wr::SnapshotTransaction second(bad_readback);
  assert(second.start(1U, credential(), emptySnapshot(768U,24U)));
  finish(second);
  assert(second.status() == wr::Status::kFault);
  assert(bad_readback.accepted_calls == 2U);
  assert(bad_readback.bytes[4096U + 28U] == 0xFFU);
}
}  // namespace

int main() {
  testV2MigrationThenV3Compaction();
  testGatewaysAndRefusal();
  testByteCutsAndSourcePreservation();
  testPendingAndLateCompletion();
  testAmbiguousAcceptedFinalActivation();
  testTamperPreactivationAndDirtyReadback();
  puts("SF5K3H real FlashBackend v3 page writer commit-last/cuts/async: PASS");
}
