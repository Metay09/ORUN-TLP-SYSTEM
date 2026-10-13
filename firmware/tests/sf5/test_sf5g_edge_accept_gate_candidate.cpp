#include "sf5g_edge_accept_gate_candidate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <initializer_list>

using namespace orun_tlp::sf5g_edge_accept_candidate;

namespace {
constexpr uint8_t kFakeRaw[] = {0xE1U, 0xDAU, 0x5AU};
constexpr size_t kRawSize = sizeof(kFakeRaw);

Attempt makeAttempt(size_t length) {
  Attempt p{};
  p.gateway_instance = 91U;
  p.attempt_id = 144U;
  p.edge_issuer = 44U;
  p.source_page = 1U;
  p.source_slot = 9U;
  p.source_generation = 27U;
  p.object_size = length;
  for (size_t i = 0U; i < length; ++i)
    p.object[i] = static_cast<uint8_t>(i * 13U + 22U);
  p.object[0] = 2U; p.object[1] = 4U; p.object[2] = 2U;
  p.object[3] = length == 84U ? 2U : 1U;
  p.object[4] = 2U; p.object[5] = 1U;
  p.object[6] = length == 84U ? 48U : 64U;
  p.object[7] = 0U;
  assert(rec::productObjectShape(p.object, p.object_size));
  return p;
}
VerifiedClaim makeClaim(const Attempt& p) {
  VerifiedClaim v{};
  v.gateway_instance = p.gateway_instance;
  v.attempt_id = p.attempt_id;
  v.edge_issuer = p.edge_issuer;
  v.source_page = p.source_page;
  v.source_slot = p.source_slot;
  v.source_generation = p.source_generation;
  v.object_size = p.object_size;
  memcpy(v.object, p.object, p.object_size);
  v.key_epoch = 3U;
  v.security_counter = 19U;
  v.scope = DurableScope::kEdgeDurableCommitted;
  return v;
}

class MockVerifier final : public ReceiptVerifier {
 public:
  VerifiedClaim value{};
  VerifyStatus status = VerifyStatus::kVerified;
  mutable size_t calls = 0U;

  VerifyStatus verify(const uint8_t* raw, size_t size,
                      VerifiedClaim& out) const override {
    ++calls;
    if (raw == nullptr || size != kRawSize ||
        memcmp(raw, kFakeRaw, kRawSize) != 0)
      return VerifyStatus::kRejected;
    if (status == VerifyStatus::kVerified) out = value;
    return status;
  }
};

// Reboot persistence is modeled by sharing durable Media between newly
// instantiated MockReplay adapters. NO real flash or crypto is involved.
struct Media {
  bool has_reservation = false;
  uint64_t issuer = 0U;
  uint32_t epoch = 0U;
  uint64_t counter = 0U;
};
class MockReplay final : public ReplayStore {
 public:
  explicit MockReplay(Media& m) : media(m) {}
  ReplayStatus override_status = ReplayStatus::kDurablyAccepted;
  size_t calls = 0U;

  ReplayStatus admitDurably(uint64_t issuer, uint32_t epoch,
                            uint64_t counter) override {
    ++calls;
    if (override_status != ReplayStatus::kDurablyAccepted)
      return override_status;
    if (media.has_reservation &&
        (issuer != media.issuer || epoch != media.epoch ||
         counter <= media.counter))
      return ReplayStatus::kReplay;
    media.has_reservation = true;
    media.issuer = issuer;
    media.epoch = epoch;
    media.counter = counter;
    return ReplayStatus::kDurablyAccepted;
  }
 private:
  Media& media;
};
GateResult run(const Attempt& a, const MockVerifier& v, ReplayStore& r,
               uint16_t pages = 4U) {
  return evaluate(a, pages, kFakeRaw, kRawSize, v, r);
}

void testValidThenReplayAfterReset() {
  for (size_t len : {size_t(84U), size_t(100U)}) {
    const Attempt pending = makeAttempt(len);
    MockVerifier verifier;
    verifier.value = makeClaim(pending);
    Media persistent;
    MockReplay first(persistent);
    assert(run(pending, verifier, first) == GateResult::kCandidateOnly);
    assert(persistent.has_reservation && persistent.counter == 19U);
    assert(first.calls == 1U && verifier.calls == 1U);

    MockReplay reboot(persistent);
    assert(run(pending, verifier, reboot) == GateResult::kReplayRejected);
    assert(reboot.calls == 1U && persistent.counter == 19U);

    Attempt new_attempt = pending;
    new_attempt.attempt_id = pending.attempt_id + 1U;
    verifier.value = makeClaim(new_attempt);
    verifier.value.security_counter = 20U;
    assert(run(new_attempt, verifier, reboot) ==
           GateResult::kCandidateOnly);
    assert(persistent.counter == 20U);
    verifier.value.security_counter = 19U;
    assert(run(new_attempt, verifier, reboot) ==
           GateResult::kReplayRejected);
  }
}

void testMockAuthenticationAndUncertainReplay() {
  Attempt pending = makeAttempt(84U);
  MockVerifier verifier;
  verifier.value = makeClaim(pending);
  Media persistent;
  MockReplay replay(persistent);
  const uint8_t spoof[] = {0xE1U, 0xDAU, 0x5BU};
  assert(evaluate(pending, 4U, spoof, sizeof(spoof), verifier, replay) ==
         GateResult::kAuthRejected);
  assert(replay.calls == 0U && !persistent.has_reservation);
  verifier.status = VerifyStatus::kRejected;
  assert(run(pending, verifier, replay) == GateResult::kAuthRejected);
  verifier.status = VerifyStatus::kUnavailable;
  assert(run(pending, verifier, replay) == GateResult::kAuthUnavailable);
  assert(replay.calls == 0U);
  verifier.status = VerifyStatus::kVerified;

  replay.override_status = ReplayStatus::kUnavailable;
  assert(run(pending, verifier, replay) == GateResult::kReplayUnavailable);
  assert(!persistent.has_reservation);
  replay.override_status = ReplayStatus::kReplay;
  assert(run(pending, verifier, replay) == GateResult::kReplayRejected);
  assert(!persistent.has_reservation);
  replay.override_status = ReplayStatus::kDurablyAccepted;
  assert(run(pending, verifier, replay) == GateResult::kCandidateOnly);
  assert(persistent.has_reservation);
}

void testBindingGuardsNoReplayConsumption() {
  const Attempt pending = makeAttempt(100U);
  MockVerifier verifier;
  Media media;
  MockReplay replay(media);
  const VerifiedClaim good = makeClaim(pending);
  verifier.value = good;

  auto expectMismatch = [&]() {
    const size_t before = replay.calls;
    assert(run(pending, verifier, replay) ==
           GateResult::kBindingMismatch);
    assert(replay.calls == before);
    assert(!media.has_reservation);
    verifier.value = good;
  };
  verifier.value.gateway_instance ^= 1U; expectMismatch();
  verifier.value.attempt_id ^= 1U; expectMismatch();
  verifier.value.edge_issuer ^= 1U; expectMismatch();
  verifier.value.source_page ^= 1U; expectMismatch();
  verifier.value.source_slot ^= 1U; expectMismatch();
  verifier.value.source_generation ^= 1U; expectMismatch();
  verifier.value.object_size = 84U; expectMismatch();
  verifier.value.object_size = 101U; expectMismatch();
  verifier.value.object[45U] ^= 1U; expectMismatch();
  verifier.value.key_epoch = 0U; expectMismatch();
  verifier.value.key_epoch = UINT32_MAX; expectMismatch();
  verifier.value.security_counter = 0U; expectMismatch();
  verifier.value.security_counter = UINT64_MAX; expectMismatch();
  verifier.value.scope = DurableScope::kTransportOnly; expectMismatch();

  assert(run(pending, verifier, replay) == GateResult::kCandidateOnly);
}

void testInvalidAttemptAndLateAccept() {
  const Attempt expected = makeAttempt(84U);
  MockVerifier verifier;
  verifier.value = makeClaim(expected);
  Media media;
  MockReplay replay(media);

  auto expectInvalid = [&](Attempt changed, uint16_t pages = 4U) {
    const size_t verified = verifier.calls;
    assert(run(changed, verifier, replay, pages) ==
           GateResult::kInvalidAttempt);
    assert(verifier.calls == verified && replay.calls == 0U);
  };
  Attempt changed = expected;
  changed.gateway_instance = 0U; expectInvalid(changed);
  changed = expected; changed.attempt_id = 0U; expectInvalid(changed);
  changed = expected; changed.edge_issuer = 0U; expectInvalid(changed);
  changed = expected; changed.source_page = 4U; expectInvalid(changed);
  changed = expected; changed.source_slot = 32U; expectInvalid(changed);
  changed = expected; changed.source_generation = 0U; expectInvalid(changed);
  changed = expected; changed.source_generation = UINT64_MAX; expectInvalid(changed);
  changed = expected; changed.object_size = 99U; expectInvalid(changed);
  changed = expected; changed.object[4U] = 0U; expectInvalid(changed);
  expectInvalid(expected, 0U);
  expectInvalid(expected, 33U);
  const size_t before = verifier.calls;
  assert(evaluate(expected, 4U, nullptr, kRawSize, verifier, replay) ==
         GateResult::kInvalidAttempt);
  assert(evaluate(expected, 4U, kFakeRaw, 0U, verifier, replay) ==
         GateResult::kInvalidAttempt);
  assert(verifier.calls == before);
  
  changed = expected;
  changed.local_fallback_started = true;
  assert(run(changed, verifier, replay) == GateResult::kFallbackInProgress);
  assert(replay.calls == 0U);
  assert(verifier.calls == before);
}
}  // namespace

int main() {
  testValidThenReplayAfterReset();
  testMockAuthenticationAndUncertainReplay();
  testBindingGuardsNoReplayConsumption();
  testInvalidAttemptAndLateAccept();
  puts("SF5G Edge accept gate: mock-auth/exact-binding/durable-replay/fallback: PASS");
}
