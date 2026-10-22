// SF5K3AB: real host issuer Ed25519 signatures over both OGK1 pages,
// strict add-only signed proposal admission, actual SF5K3Y snapshot reader.
// No grant writer, secure-element revision, RF, or flash mutation.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_signed_grant_append_preflight.h"

#define ORUN_SF5K3AB_REUSE_SF5K3Z_FIXTURE
#include "test_sf5k3z_gateway_issuer_rollback.cpp"
#undef ORUN_SF5K3AB_REUSE_SF5K3Z_FIXTURE

namespace {
using Gate = orun_tlp::GatewaySignedGrantAppendPreflight;
using V = Gate::Verdict;

void createSigned(Nor& flash, const Issuer& issuer, uint64_t revision,
                  bool append, bool alter_prior, bool remove_prior,
                  uint64_t id = kGateway) {
  makePage(flash, 0U, id, revision);
  uint8_t* page = flash.bytes.data();
  if (remove_prior) {
    memset(page + fmt::kHeaderBytes, 0U, fmt::kGrantBytes);
  }
  if (alter_prior)
    page[fmt::kHeaderBytes + 24U] ^= 0x01U;
  if (append) {
    placeGrant(page, 1U, kTracker + 1U, 7U, 10U, 4U, 31U);
  }
  issuer.sign(flash, 0U);
}

void testAppendAndLiveRevocation() {
  Issuer issuer;
  RealEd25519Verifier verifier(issuer.public_key);
  Nor old_flash, proposal_flash;
  createSigned(old_flash, issuer, 7U, false, false, false);
  createSigned(proposal_flash, issuer, 8U, true, false, false);

  GatewayGrantSnapshotReader original(old_flash, verifier);
  GatewayGrantSnapshotReader next(proposal_flash, verifier);
  assert(original.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(next.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  RevisionAnchor anchor;
  anchor.revision = 7U;
  assert(Gate::inspect(original, next, anchor, kGateway) ==
         V::kEligibleForSeparateCommitReview);

  anchor.revision = 8U;  // Old flash is stale against independent checkpoint
  assert(Gate::inspect(original, next, anchor, kGateway) ==
         V::kAnchorMismatch);
  anchor.revision = 7U;
  anchor.trusted = false;
  assert(Gate::inspect(original, next, anchor, kGateway) ==
         V::kAnchorUnavailable);
  anchor.trusted = true;

  // Explicitly killing an in-RAM grant source defeats stale future reviews.
  original.denyForCurrentBoot();
  assert(Gate::inspect(original, next, anchor, kGateway) ==
         V::kCurrentUnverified);
  assert(old_flash.writes == 0U && old_flash.erases == 0U);
  assert(proposal_flash.writes == 0U && proposal_flash.erases == 0U);
}

void testScopeMutationBlocked() {
  Issuer issuer;
  RealEd25519Verifier verifier(issuer.public_key);
  const struct Case {
    uint64_t revision;
    bool append, alter_prior, remove_prior;
    V expected;
  } cases[] = {
      {8U, false, false, false, V::kNoNewTracker},
      {7U, true, false, false, V::kNotNextRevision},
      {9U, true, false, false, V::kNotNextRevision},
      {8U, true, true, false, V::kExistingGrantChanged},
      {8U, true, false, true, V::kExistingGrantChanged},
  };

  for (const auto& row : cases) {
    Nor current_flash, staged_flash;
    createSigned(current_flash, issuer, 7U, false, false, false);
    createSigned(staged_flash, issuer, row.revision, row.append,
                 row.alter_prior, row.remove_prior);
    GatewayGrantSnapshotReader current(current_flash, verifier);
    GatewayGrantSnapshotReader candidate(staged_flash, verifier);
    assert(current.begin(kGateway, true, false) ==
           GatewayGrantSnapshotReader::Status::kReady);
    assert(candidate.begin(kGateway, true, false) ==
           GatewayGrantSnapshotReader::Status::kReady);
    RevisionAnchor anchor;
    anchor.revision = 7U;
    assert(Gate::inspect(current, candidate, anchor, kGateway) ==
           row.expected);
    assert(current_flash.writes == 0U && current_flash.erases == 0U);
    assert(staged_flash.writes == 0U && staged_flash.erases == 0U);
  }
}

void testUnsignedInvalidIssuerAndIdentity() {
  Issuer issuer;
  Issuer other;
  RealEd25519Verifier verifier(issuer.public_key);
  Nor current_flash, staged_flash;
  createSigned(current_flash, issuer, 7U, false, false, false);
  createSigned(staged_flash, issuer, 8U, true, false, false);
  // Silently change a scoped ACK secret; repair unkeyed CRC only.
  staged_flash.bytes[fmt::kHeaderBytes + 24U] ^= 1U;
  be32(staged_flash.bytes.data() + fmt::kCrcOffset,
       observation_store_format::crc32(
           staged_flash.bytes.data(), fmt::kCrcOffset));
  GatewayGrantSnapshotReader current(current_flash, verifier);
  GatewayGrantSnapshotReader tampered(staged_flash, verifier);
  assert(current.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(tampered.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kRejected);
  RevisionAnchor anchor;
  anchor.revision = 7U;
  assert(Gate::inspect(current, tampered, anchor, kGateway) ==
         V::kCandidateUnverified);

  Nor impostor_flash;
  createSigned(impostor_flash, other, 8U, true, false, false);
  GatewayGrantSnapshotReader impostor(impostor_flash, verifier);
  assert(impostor.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kRejected);
  assert(Gate::inspect(current, impostor, anchor, kGateway) ==
         V::kCandidateUnverified);

  Nor other_gateway_flash;
  createSigned(other_gateway_flash, issuer, 8U, true, false, false,
               kGateway + 1U);
  GatewayGrantSnapshotReader other_gateway(other_gateway_flash, verifier);
  assert(other_gateway.begin(kGateway + 1U, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(Gate::inspect(current, other_gateway, anchor, kGateway) ==
         V::kWrongGateway);
  assert(Gate::inspect(current, current, anchor, 0U) == V::kWrongGateway);
  assert(Gate::inspect(current, current, anchor, kGateway) ==
         V::kNotNextRevision);
  assert(staged_flash.writes == 0U && staged_flash.erases == 0U);
}

}  // namespace

int main() {
  testAppendAndLiveRevocation();
  testScopeMutationBlocked();
  testUnsignedInvalidIssuerAndIdentity();
  puts("SF5K3AB signed append-only Gateway enrollment preflight: PASS (HOST only)");
  return 0;
}
