// HOST-ONLY decisions for two *separate* physical Flash stores.
// No real ACK verification or Flash authorisation is present in this test.
#include <assert.h>
#include <stdio.h>

#include "kq05_dual_region_decisions.h"

namespace k = orun_tlp::kq05_dual_region;

namespace {

k::AppendInput fullyBound(k::Role role) {
  k::AppendInput v{};
  v.role = role;
  v.primary = k::Store::kFull;
  v.supplemental = k::Store::kReady;
  v.primary_duplicate = k::Proof::kAbsent;
  v.supplemental_duplicate = k::Proof::kAbsent;
  v.legacy_writer_disabled = true;
  v.supplemental_identity_bound = true;
  v.shared_tracker_sequence_owner = true;
  return v;
}

void testPrimaryUntilFull() {
  k::AppendInput v = fullyBound(k::Role::kTracker);
  v.primary = k::Store::kReady;
  assert(k::selectAppend(v).decision == k::Decision::kPrimary);
  assert(k::selectAppend(v).region == k::Region::kPrimary128K);
  v.primary = k::Store::kFull;
  assert(k::selectAppend(v).decision == k::Decision::kSupplemental);
  assert(k::selectAppend(v).region == k::Region::kSupplemental28K);
  v.supplemental = k::Store::kFull;
  assert(k::selectAppend(v).decision == k::Decision::kFull);
  assert(k::selectAppend(v).region == k::Region::kNone);
}

void testNoLegacyOverlapOrDuplicateIdentity() {
  k::AppendInput v = fullyBound(k::Role::kTracker);
  v.legacy_writer_disabled = false;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.legacy_writer_disabled = true;
  v.supplemental_identity_bound = false;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.supplemental_identity_bound = true;
  v.shared_tracker_sequence_owner = false;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.shared_tracker_sequence_owner = true;
  v.supplemental_duplicate = k::Proof::kUncertain;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.supplemental_duplicate = k::Proof::kExactDurable;
  assert(k::selectAppend(v).decision == k::Decision::kAlreadyPresent);
  v.supplemental_duplicate = k::Proof::kAbsent;
  v.primary_duplicate = k::Proof::kExactDurable;
  assert(k::selectAppend(v).decision == k::Decision::kAlreadyPresent);
  v.primary_duplicate = k::Proof::kDifferent;
  assert(k::selectAppend(v).decision == k::Decision::kConflict);
}

void testGatewayRequiresBindingButNotSharedTrackerSequence() {
  k::AppendInput gw = fullyBound(k::Role::kGateway);
  gw.shared_tracker_sequence_owner = false;
  assert(k::selectAppend(gw).decision == k::Decision::kSupplemental);
  gw.supplemental_identity_bound = false;
  assert(k::selectAppend(gw).decision == k::Decision::kBlocked);
  gw.supplemental_identity_bound = true;
  gw.supplemental = k::Store::kFault;
  assert(k::selectAppend(gw).decision == k::Decision::kFault);
  gw.supplemental = k::Store::kUnavailable;
  assert(k::selectAppend(gw).decision == k::Decision::kBlocked);
  gw.supplemental = k::Store::kBusy;
  assert(k::selectAppend(gw).decision == k::Decision::kBusy);
}

void testNoMutationOnUnavailablePrimaryOrRole() {
  k::AppendInput v = fullyBound(k::Role::kGateway);
  v.role = k::Role::kConflict;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.role = k::Role::kUnassigned;
  assert(k::selectAppend(v).decision == k::Decision::kBlocked);
  v.role = k::Role::kGateway;
  v.primary = k::Store::kFault;
  assert(k::selectAppend(v).decision == k::Decision::kFault);
  v.primary = k::Store::kBusy;
  assert(k::selectAppend(v).decision == k::Decision::kBusy);
  v.primary = k::Store::kUnavailable;
  assert(k::selectAppend(v).decision == k::Decision::kBusy);
}

void testOnlyAuthenticatedExactRelease() {
  k::ReleaseInput v{};
  v.role = k::Role::kTracker;
  v.primary = k::Proof::kExactDurable;
  v.supplemental = k::Proof::kAbsent;
  v.external_proof_authenticated = true;
  v.scope_matches = true;
  assert(k::selectRelease(v).decision == k::Decision::kPrimary);
  assert(k::selectRelease(v).region == k::Region::kPrimary128K);
  v.primary = k::Proof::kAbsent;
  v.supplemental = k::Proof::kExactDurable;
  assert(k::selectRelease(v).decision == k::Decision::kSupplemental);
  assert(k::selectRelease(v).region == k::Region::kSupplemental28K);

  // A packet that is merely structurally valid MUST NOT authorize release.
  v.external_proof_authenticated = false;
  assert(k::selectRelease(v).decision == k::Decision::kBlocked);
  v.external_proof_authenticated = true;
  v.scope_matches = false;
  assert(k::selectRelease(v).decision == k::Decision::kBlocked);

  v.scope_matches = true;
  v.primary = k::Proof::kExactDurable;
  assert(k::selectRelease(v).decision == k::Decision::kConflict);
  v.primary = k::Proof::kUncertain;
  assert(k::selectRelease(v).decision == k::Decision::kBlocked);
  v.primary = k::Proof::kDifferent;
  assert(k::selectRelease(v).decision == k::Decision::kConflict);

  v.role = k::Role::kGateway;
  v.primary = k::Proof::kAbsent;
  assert(k::selectRelease(v).decision == k::Decision::kSupplemental);
  v.role = k::Role::kConflict;
  assert(k::selectRelease(v).decision == k::Decision::kBlocked);
}

}  // namespace

int main() {
  testPrimaryUntilFull();
  testNoLegacyOverlapOrDuplicateIdentity();
  testGatewayRequiresBindingButNotSharedTrackerSequence();
  testNoMutationOnUnavailablePrimaryOrRole();
  testOnlyAuthenticatedExactRelease();
  puts("KQ05 offline dual-region append/release routing: PASS (HOST DECISIONS ONLY)");
  puts("NO NOR WRITES, NO NEW CAPACITY ENABLED, NO ACK MINTED");
  return 0;
}
