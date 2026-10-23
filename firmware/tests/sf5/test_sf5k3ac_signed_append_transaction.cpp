// SF5K3AC: TEST-ONLY Gateway signed A/B enrollment write simulation.
// Real Ed25519 issuer proof, actual OGK1 reader, fake NOR, mocked trusted
// rollback anchor. Absolutely no field Gateway hardware/flash activation.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_signed_grant_append_transaction.h"

#define ORUN_SF5K3AC_REUSE_AB_FIXTURE
#include "test_sf5k3ab_signed_append_preflight.cpp"
#undef ORUN_SF5K3AC_REUSE_AB_FIXTURE

namespace {
using Txn = orun_tlp::GatewaySignedGrantAppendTransaction;
using Reader = orun_tlp::GatewayGrantSnapshotReader;

struct WritableNor final : orun_tlp::FlashBackend {
  std::array<uint8_t, fmt::kPageBytes * fmt::kPageCount> bytes{};
  enum class Deferred : uint8_t { kNone, kErase, kProgram };
  Deferred deferred = Deferred::kNone;
  uint32_t deferred_offset = 0U;
  uint16_t deferred_page = 0U;
  uint8_t deferred_bytes[64]{};
  size_t deferred_length = 0U;
  bool delay_next = false;
  bool read_ok = true;
  bool unreconciled = false;
  unsigned programs = 0U, erases = 0U, polls = 0U;
  unsigned reject_program_after = UINT32_MAX;

  WritableNor() { bytes.fill(0xFFU); }

  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t length) const override {
    if (!read_ok || out == nullptr || offset > bytes.size() ||
        length > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, length);
    return true;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }

  orun_tlp::FlashOpResult applyProgram(uint32_t offset,
                                       const uint8_t* input, size_t n) {
    if (programs >= reject_program_after || input == nullptr ||
        n == 0U || n > 64U || (offset & 3U) || (n & 3U) ||
        offset > bytes.size() || n > bytes.size() - offset)
      return orun_tlp::FlashOpResult::kFailed;
    for (size_t i = 0U; i < n; ++i)
      if (bytes[offset + i] != 0xFFU) return orun_tlp::FlashOpResult::kFailed;
    ++programs;
    for (size_t i = 0U; i < n; ++i) bytes[offset + i] &= input[i];
    return orun_tlp::FlashOpResult::kDone;
  }
  orun_tlp::FlashOpResult program(uint32_t offset, const void* in,
                                  size_t n) override {
    if (deferred != Deferred::kNone) return orun_tlp::FlashOpResult::kFailed;
    if (delay_next) {
      if (!in || n == 0U || n > 64U || (n & 3U) != 0U ||
          offset > bytes.size() || n > bytes.size() - offset)
        return orun_tlp::FlashOpResult::kFailed;
      delay_next = false;
      deferred = Deferred::kProgram;
      deferred_offset = offset;
      deferred_length = n;
      memcpy(deferred_bytes, in, n);
      return orun_tlp::FlashOpResult::kPending;
    }
    return applyProgram(offset, static_cast<const uint8_t*>(in), n);
  }
  orun_tlp::FlashOpResult erasePage(uint32_t page) override {
    if (page >= fmt::kPageCount ||
        deferred != Deferred::kNone) return orun_tlp::FlashOpResult::kFailed;
    if (delay_next) {
      delay_next = false;
      deferred = Deferred::kErase;
      deferred_page = static_cast<uint16_t>(page);
      return orun_tlp::FlashOpResult::kPending;
    }
    ++erases;
    memset(bytes.data() + page * fmt::kPageBytes, 0xFF, fmt::kPageBytes);
    return orun_tlp::FlashOpResult::kDone;
  }
  orun_tlp::FlashOpResult pollPending() override {
    ++polls;
    const auto op = deferred;
    deferred = Deferred::kNone;
    if (op == Deferred::kErase) return erasePage(deferred_page);
    if (op == Deferred::kProgram)
      return applyProgram(deferred_offset, deferred_bytes, deferred_length);
    return orun_tlp::FlashOpResult::kFailed;
  }
};

struct Inputs final {
  Issuer issuer;
  RealEd25519Verifier verifier;
  Nor signed_old;
  Nor signed_new;
  WritableNor actual;
  Reader current;
  Reader proposal;
  RevisionAnchor anchor;
  Txn transaction;

  Inputs()
      : verifier(issuer.public_key), current(actual, verifier),
        proposal(signed_new, verifier), transaction(actual) {
    createSigned(signed_old, issuer, 7U, false, false, false);
    createSigned(signed_new, issuer, 8U, true, false, false);
    actual.bytes = signed_old.bytes;
    anchor.revision = 7U;
  }
  void mount() {
    assert(current.begin(kGateway, true, false) == Reader::Status::kReady);
    assert(proposal.begin(kGateway, true, false) == Reader::Status::kReady);
  }
  bool start(bool exclusive = true, bool rf_quiet = true) {
    return transaction.start(current, proposal, anchor, kGateway,
                             exclusive, rf_quiet);
  }
};

void settle(Inputs& in) {
  for (unsigned i = 0U; i < 200U &&
       in.transaction.state() == Txn::State::kBusy; ++i) {
    in.transaction.poll();
  }
}

void testSuccessfulWriteButNoAuthorization() {
  Inputs in;
  in.mount();
  const auto old = in.signed_old.bytes;
  assert(in.start());
  assert(!in.current.verifiedForLocalGateway(kGateway));
  settle(in);
  assert(in.transaction.state() == Txn::State::kCommittedAwaitingAnchor);
  assert(in.actual.programs > 0U && in.actual.erases == 1U);
  assert(memcmp(in.actual.bytes.data(), old.data(),
                fmt::kPageBytes) == 0);

  Reader boot(in.actual, in.verifier);
  assert(boot.begin(kGateway, true, false) == Reader::Status::kReady);
  assert(boot.activeGeneration() == 8U);
  orun_tlp::GatewayAnchoredGrantSource authority(boot, in.anchor);
  assert(!authority.verifiedForLocalGateway(kGateway));
  GatewayCustodyAckScope scope{};
  uint8_t key[32]{};
  assert(!authority.activeGrant(kTracker + 1U, 7U, scope, key));
  in.anchor.revision = 8U;  // MOCKED independent secure-element advancement
  assert(authority.verifiedForLocalGateway(kGateway));
  assert(authority.activeGrant(kTracker + 1U, 7U, scope, key));
  assert(scope.gateway_device_id == kGateway);
  assert(key[0] == 31U);
}

void testRefuseBeforeAnyMutation() {
  Inputs in;
  in.mount();
  assert(!in.start(false, true));
  assert(!in.start(true, false));
  in.anchor.revision = 6U;
  assert(!in.start());
  in.anchor.revision = 7U;
  assert(in.actual.erases == 0U && in.actual.programs == 0U);
  assert(in.current.verifiedForLocalGateway(kGateway));

  Issuer other;
  RealEd25519Verifier untrusted(other.public_key);
  Nor forged;
  createSigned(forged, other, 8U, true, false, false);
  Reader wrong_issuer(forged, untrusted);
  assert(wrong_issuer.begin(kGateway, true, false) ==
         Reader::Status::kReady);
  assert(!in.transaction.start(in.current, wrong_issuer, in.anchor,
                               kGateway, true, true));
  assert(in.actual.erases == 0U && in.actual.programs == 0U);
}

void testTornPowerCutRefusesBoot() {
  Inputs in;
  in.mount();
  const auto old = in.signed_old.bytes;
  assert(in.start());
  assert(in.transaction.poll() == Txn::State::kBusy); // erase inactive
  assert(in.transaction.poll() == Txn::State::kBusy); // first body chunk
  assert(in.actual.programs == 1U && in.actual.erases == 1U);
  assert(memcmp(in.actual.bytes.data(), old.data(),
                fmt::kPageBytes) == 0);
  Reader reboot(in.actual, in.verifier);
  assert(reboot.begin(kGateway, true, false) ==
         Reader::Status::kRejected);
  assert(!reboot.verifiedForLocalGateway(kGateway));
  assert(in.actual.bytes[fmt::kPageBytes + fmt::kCommitOffset] == 0xFFU);
}

void testMidWriteFlashFailure() {
  Inputs in;
  in.mount();
  in.actual.reject_program_after = 3U;
  assert(in.start());
  settle(in);
  assert(in.transaction.state() == Txn::State::kFault);
  assert(in.actual.erases == 1U && in.actual.programs == 3U);
  assert(in.actual.bytes[fmt::kPageBytes + fmt::kCommitOffset] == 0xFFU);
  assert(!in.current.verifiedForLocalGateway(kGateway));
  assert(in.transaction.poll() == Txn::State::kFault);
}

void testRevokedAnchorDuringPrepare() {
  Inputs in;
  in.mount();
  assert(in.start());
  for (unsigned i = 0U; i < 100U && in.actual.programs < 30U; ++i)
    in.transaction.poll();
  assert(in.actual.programs == 30U);
  in.anchor.revision = 8U;
  settle(in);
  assert(in.transaction.state() == Txn::State::kFault);
  assert(in.actual.bytes[fmt::kPageBytes + fmt::kCommitOffset] == 0xFFU);
}

void testAnchorChangesAfterPrecommit() {
  Inputs in;
  in.mount();
  assert(in.start());
  assert(in.transaction.poll() == Txn::State::kBusy); // erase
  for (unsigned i = 0U; i < 30U; ++i)
    assert(in.transaction.poll() == Txn::State::kBusy); // body
  assert(in.actual.programs == 30U);
  assert(in.transaction.poll() == Txn::State::kBusy); // enter precommit
  assert(in.transaction.poll() == Txn::State::kBusy); // enter commit
  // This change happened AFTER the first verification but BEFORE the
  // actual one-way commit program. The writer must re-check and abort.
  in.anchor.revision = 8U;
  assert(in.transaction.poll() == Txn::State::kFault);
  assert(in.actual.bytes[fmt::kPageBytes + fmt::kCommitOffset] == 0xFFU);
  assert(in.actual.programs == 30U);
}

void testAsyncCompletionNoDoubleSubmission() {
  Inputs in;
  in.mount();
  in.actual.delay_next = true;  // first erasePending()
  assert(in.start());
  assert(in.transaction.poll() == Txn::State::kBusy);
  assert(in.actual.erases == 0U && in.actual.polls == 0U);
  assert(in.transaction.poll() == Txn::State::kBusy);
  assert(in.actual.erases == 1U && in.actual.polls == 1U);
  in.actual.delay_next = true;  // first programPending()
  assert(in.transaction.poll() == Txn::State::kBusy);
  assert(in.actual.programs == 0U);
  assert(in.transaction.poll() == Txn::State::kBusy);
  assert(in.actual.programs == 1U && in.actual.polls == 2U);
  settle(in);
  assert(in.transaction.state() == Txn::State::kCommittedAwaitingAnchor);
  assert(in.actual.erases == 1U);
}

}  // namespace

int main() {
  // Existing issuer/rollback/append host regressions remain executed.
  issuerSignatureAndRollback();
  rejectForgedOrWrongIssuer();
  testAppendAndLiveRevocation();
  testScopeMutationBlocked();
  testUnsignedInvalidIssuerAndIdentity();

  testSuccessfulWriteButNoAuthorization();
  testRefuseBeforeAnyMutation();
  testTornPowerCutRefusesBoot();
  testMidWriteFlashFailure();
  testRevokedAnchorDuringPrepare();
  testAnchorChangesAfterPrecommit();
  testAsyncCompletionNoDoubleSubmission();
  puts("SF5K3AC signed Gateway A/B staging simulation: PASS (HOST ONLY)");
  return 0;
}
