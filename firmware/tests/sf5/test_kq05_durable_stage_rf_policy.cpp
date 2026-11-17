// KQ05: only after authentic Gateway-local durable STAGED readback may an
// exact Backend receipt be considered for LoRa scheduling. PC fake radio only.
//
// This test reuses the existing real-byte Python->BLE->native offer verifier;
// it DOES NOT use the actual RadioManager (whose command API rejects batch
// receipts), install a physical enrollment, or certify on-air RF budgets.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "custody_ack_primitives.h"

// Rename the existing interop program's entrypoint; reuse precisely its
// independently compiled real-byte BLE/HMAC checks and public test credential.
#define main kq05_existing_interop_main
#include "test_kq05_python_gateway_interop.cpp"
#undef main

namespace {
constexpr char kStageDomain[] = "ORUN-KQ05-GW-EDGE-STAGED-RESULT-v1";
constexpr char kEdgeId[] = "edge-one";
constexpr size_t kUnsignedStageSize = 51U;
constexpr size_t kStageMacInputSize =
    sizeof(kStageDomain) + sizeof(kEdgeId) + kUnsignedStageSize;

bool verifiesDurableStage(const uint8_t* logical_offer, size_t offer_len,
                         const uint8_t* stage, size_t stage_len) {
  if (logical_offer == nullptr || stage == nullptr || offer_len < 2U ||
      stage_len != 84U) return false;
  wire::UntrustedOfferView offered{};
  wire::UntrustedStageView proof{};
  if (!wire::inspectOffer(logical_offer, offer_len, offered)) {
    fprintf(stderr, "KQ05 STAGED: OFFER_SHAPE rejected size=%zu\\n", offer_len);
    return false;
  }
  if (!wire::inspectStageResult(stage, stage_len, proof)) {
    fprintf(stderr, "KQ05 STAGED: RESPONSE_SHAPE rejected size=%zu\\n", stage_len);
    return false;
  }
  if (proof.source_row != offered.source_row ||
      memcmp(proof.gateway, offered.gateway, 8U) != 0) {
    fprintf(stderr, "KQ05 STAGED: GATEWAY_OR_ROW mismatch\\n");
    return false;
  }

  uint8_t digest[32]{};
  if (!orun_tlp::custody_crypto::sha256(
          logical_offer + 1U, offer_len - 1U, digest)) {
    fprintf(stderr, "KQ05 STAGED: SHA256 unavailable size=%zu\\n",
            offer_len - 1U);
    return false;
  }
  if (memcmp(proof.offer_sha256, digest, sizeof(digest)) != 0) {
    fprintf(stderr, "KQ05 STAGED: OFFER_DIGEST mismatch\\n");
    return false;
  }
  // Domain includes a NUL. Edge ID is followed by a separate NUL.
  uint8_t mac_input[kStageMacInputSize]{};
  memcpy(mac_input, kStageDomain, sizeof(kStageDomain));
  memcpy(mac_input + sizeof(kStageDomain), kEdgeId, sizeof(kEdgeId));
  memcpy(mac_input + sizeof(kStageDomain) + sizeof(kEdgeId),
         stage + 1U, kUnsignedStageSize);
  uint8_t tag[32]{};
  if (!orun_tlp::custody_crypto::hmacSha256Bounded(
          kPublicDummyLink, mac_input, sizeof(mac_input), tag)) {
    fprintf(stderr, "KQ05 STAGED: HMAC unavailable input_size=%zu\\n",
            sizeof(mac_input));
    return false;
  }
  if (memcmp(tag, proof.link_mac, sizeof(tag)) != 0) {
    fprintf(stderr,
            "KQ05 STAGED: HMAC mismatch input=%zu domain=%zu edge=%zu\\n",
            sizeof(mac_input), sizeof(kStageDomain), sizeof(kEdgeId));
    return false;
  }
  return true;
}

relay::TxConditions eligible() {
  relay::TxConditions p{};
  p.gateway_service_authorized = true;
  p.radio_idle = true;
  p.custody_ack_pending = false;
  p.command_pending = false;
  p.live_pending = false;
  p.regional_rf_budget_granted = true;
  p.tracker_rx_window_authorized = true;
  p.heard_tracker = kTracker;
  p.heard_age_ms = 600U;
  return p;
}

void deniedWhenPriorityOrBudgetUnavailable(
    relay::GatewayRamRelay& q, FakeSignedEnrollment& credentials) {
  for (unsigned n = 0U; n < 8U; ++n) {
    auto policy = eligible();
    switch (n) {
      case 0U: policy.radio_idle = false; break;
      case 1U: policy.custody_ack_pending = true; break;
      case 2U: policy.command_pending = true; break;
      case 3U: policy.live_pending = true; break;
      case 4U: policy.regional_rf_budget_granted = false; break;
      case 5U: policy.tracker_rx_window_authorized = false; break;
      case 6U: policy.heard_tracker = kGateway; break;
      case 7U: policy.heard_age_ms = 3001U; break;
    }
    relay::Candidate chosen{};
    chosen.source_row = 999U;
    assert(!q.select(1500U + n, policy, credentials, chosen));
    assert(chosen.source_row == 0U && chosen.length == 0U);
    assert(q.pending(1510U) == 1U); // Not consumed, no fake ACK.
  }
}

void testDurableDispatch(const uint8_t* offer, size_t offer_size,
                         const uint8_t* proof, size_t proof_size) {
  assert(offer_size == 96U && proof_size == 84U);
  // The BLE envelope 0x08 must NOT be confused with the authenticated inner
  // 0x92 STAGED type. Python and native firmware must agree byte for byte.
  assert(proof[0] == wire::kStageEnvelopeType);
  assert(proof[1] == wire::kVersion);
  assert(proof[2] == wire::kInnerStageType);
  assert(verifiesDurableStage(offer, offer_size, proof, proof_size));

  uint8_t tampered[160]{};
  memcpy(tampered, proof, proof_size);
  tampered[0] = wire::kInnerStageType;  // Outer/inner confusion is invalid.
  assert(!verifiesDurableStage(offer, offer_size, tampered, proof_size));
  memcpy(tampered, proof, proof_size);
  tampered[proof_size - 1U] ^= 0x01U;
  assert(!verifiesDurableStage(offer, offer_size, tampered, proof_size));
  memcpy(tampered, proof, proof_size);
  tampered[1U + 19U] ^= 0x40U; // Exact-offer digest corruption.
  assert(!verifiesDurableStage(offer, offer_size, tampered, proof_size));
  assert(!verifiesDurableStage(offer, offer_size, proof, proof_size - 1U));

  FakeSignedEnrollment cred;
  relay::GatewayRamRelay queue(kGateway);

  // The Python fixture emitted this offer ONLY after independently durable
  // Gateway SQLite commit/readback and an authenticated 0x92 stage result.
  assert(queue.accept(offer, offer_size, 1000U, true, cred) ==
         relay::Admission::kQueued);
  deniedWhenPriorityOrBudgetUnavailable(queue, cred);

  // Even a policy-eligible selection is NOT a radio launch, durable Tracker
  // receipt, backend deletion, or a release marker.
  relay::Candidate next{};
  assert(queue.select(1600U, eligible(), cred, next));
  assert(next.tracker == kTracker && next.source_row == 1U &&
         next.length == 44U);
  assert(queue.pending(1601U) == 1U);
  assert(memcmp(next.wire, offer + 1U + 19U, next.length) == 0);

  bool fake_radio_accepts = false;
  if (fake_radio_accepts) queue.markTxStarted(next.source_row, 1601U);
  assert(queue.select(1602U, eligible(), cred, next));
  fake_radio_accepts = true;
  if (fake_radio_accepts) queue.markTxStarted(next.source_row, 1603U);
  assert(!queue.select(1604U, eligible(), cred, next));
  assert(next.length == 0U); // Retry pacing after actual launch only.

  // The RAM queue has a 20-second TTL: it does NOT keep entries long enough
  // for its 30-second retry delay. Redelivery must come from durable stage.
  assert(queue.pending(21001U) == 0U);
  assert(!queue.select(21001U, eligible(), cred, next));
  assert(queue.accept(offer, offer_size, 22000U, true, cred) ==
         relay::Admission::kQueued);
  assert(queue.select(22100U, eligible(), cred, next));
  assert(next.length == 44U);

  // An issuer or local Edge-enrollment revocation must fail closed.
  cred.signed_snapshot = false;
  assert(!queue.select(22200U, eligible(), cred, next));
  assert(queue.pending(22201U) == 0U);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 3);
  uint8_t offer[160]{};
  uint8_t proof[160]{};
  const size_t offer_len = readFixture(argv[1], offer);
  const size_t proof_len = readFixture(argv[2], proof);
  testInterop(offer, offer_len); // existing native BLE/HMAC regression
  testDurableDispatch(offer, offer_len, proof, proof_len);
  puts("KQ05 real durable Gateway STAGED -> native receipt RF priority/window/budget: PASS (HOST FAKE RADIO ONLY)");
  return 0;
}
