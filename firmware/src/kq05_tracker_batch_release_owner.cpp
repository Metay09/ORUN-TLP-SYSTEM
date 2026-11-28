#if defined(ORUN_GATEWAY_BATCH_ACK) || defined(ORUN_TRACKER_BATCH_ACK)
#include "kq05_tracker_batch_release_owner.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "security_store.h"

namespace orun_tlp {
namespace {
// Reuse orun_tlp::osf / orun_tlp::osc from observation_store.h.
// Local anonymous-namespace aliases conflict with them on GCC 7.

void wipe(void* memory, size_t n) {
  volatile uint8_t* p=static_cast<volatile uint8_t*>(memory);
  while (n-- != 0U) *p++=0U;
}
bool sameIdentity(const osf::RecordIdentity& a,
                  const osf::RecordIdentity& b) {
  return a.incarnation==b.incarnation && a.sequence==b.sequence;
}
bool sameDigest(const uint8_t (&expected)[32],
                const uint8_t (&actual)[32]) {
  volatile uint8_t diff=0U;
  for (size_t i=0U;i<sizeof(actual);++i)
    diff=static_cast<uint8_t>(diff | (expected[i]^actual[i]));
  return diff==0U;
}
} // namespace

void Kq05TrackerBatchReleaseOwner::clear() {
  wipe(&proof_,sizeof(proof_));
  proof_=AuthenticatedKq05TrackerBatchAck{};
  index_=0U;
  active_=false;
  waiting_flash_=false;
  // Keep released_ as a report of the most recently completed operation.
}

bool Kq05TrackerBatchReleaseOwner::currentGrant() const {
  return proof_.verified_ &&
      proof_.count_ >= 1U &&
      proof_.count_ <= kq05_gateway_batch_ack::kMaxObjects &&
      security_.custodyAckScopeMatches(
          proof_.tracker_id_,proof_.gateway_id_,
          proof_.key_epoch_,proof_.floor_,proof_.generation_);
}

bool Kq05TrackerBatchReleaseOwner::currentExact(
    uint8_t index, osc::ExactObject& value) const {
  if (!active_ || !currentGrant() || index>=proof_.count_) return false;
  const osf::RecordIdentity& id=proof_.identities_[index];
  if (!id.valid() ||
      records_.findCachedExact(id,value) !=
          ObservationStore::ControlLookupResult::kFound ||
      !sameIdentity(value.identity,id) ||
      value.object_size==0U || value.object_size>sizeof(value.object) ||
      !records_.custodyCandidateMatchesRetainedExact(value))
    return false;

  uint8_t digest[32]{};
  const bool calculated=custody_crypto::sha256(
      value.object,value.object_size,digest);
  const bool exact=calculated &&
      sameDigest(proof_.full_digests_[index],digest);
  wipe(digest,sizeof(digest));
  return exact && currentGrant();
}

Kq05TrackerBatchReleaseOwner::Submit
Kq05TrackerBatchReleaseOwner::submit(const uint8_t* frame,size_t length) {
  if (active_) return Submit::kBusy;
  if (records_.preparing() || records_.pending() ||
      !records_.mounted() || !records_.store().ready() ||
      records_.store().faulted() || records_.store().busy())
    return Submit::kStorageUnavailable;

  AuthenticatedKq05TrackerBatchAck verified{};
  const Kq05TrackerBatchVerifyResult v=
      verifier_.verifyFromStore(frame,length,records_,verified);
  if (v==Kq05TrackerBatchVerifyResult::kStoreUnavailable)
    return Submit::kStorageUnavailable;
  if (v!=Kq05TrackerBatchVerifyResult::kAuthenticated ||
      !verified.verified_ || verified.count_==0U ||
      verified.count_>kq05_gateway_batch_ack::kMaxObjects)
    return Submit::kAuthenticationRejected;

  // Full group authentication must precede even the FIRST Flash mutation.
  // All matching identities + digests come only from the private verifier.
  proof_=verified;
  if (!currentGrant()) {
    clear();
    return Submit::kAuthenticationRejected;
  }
  for (uint8_t i=0U;i<proof_.count_;++i) {
    for (uint8_t j=0U;j<i;++j)
      if (sameIdentity(proof_.identities_[i],proof_.identities_[j])) {
        clear();
        return Submit::kAuthenticationRejected;
      }
  }
  index_=0U;
  released_=0U;
  waiting_flash_=false;
  active_=true;
  return Submit::kStarted;
}

Kq05TrackerBatchReleaseOwner::Service
Kq05TrackerBatchReleaseOwner::service() {
  if (!active_) return Service::kIdle;

  if (waiting_flash_) {
    osf::RecordIdentity committed{};
    bool durable=false;
    if (!records_.takeCustodyReleaseOutcome(committed,durable))
      return Service::kWaitingFlash;

    // The marker may have been durably written before revocation; that
    // cannot be undone. Never start the NEXT marker if the grant changed.
    const bool correct=index_<proof_.count_ &&
        sameIdentity(committed,proof_.identities_[index_]);
    waiting_flash_=false;
    if (!correct || !durable) {
      clear();
      return Service::kFailed;
    }
    ++released_;
    ++index_;
    if (!currentGrant()) {
      clear();
      return Service::kFailed;
    }
    // Deliberately yield after each committed marker. This permits
    // scheduler fairness with live positions and policy changes.
    return Service::kReleasedOne;
  }

  if (index_>=proof_.count_) {
    clear();
    return Service::kApplied;
  }
  if (!currentGrant()) {
    clear();
    return Service::kFailed;
  }
  if (records_.preparing() || records_.pending() ||
      records_.store().busy() || !records_.store().ready() ||
      records_.store().faulted())
    return Service::kWaitingStorage;

  osc::ExactObject exact{};
  if (!currentExact(index_,exact)) {
    // The previously authenticated item changed or is no longer
    // retainable. Do not infer acceptance for any other item.
    wipe(&exact,sizeof(exact));
    clear();
    return Service::kFailed;
  }
  if (!records_.requestAuthenticatedCustodyRelease(exact)) {
    wipe(&exact,sizeof(exact));
    if (records_.store().busy() || records_.pending())
      return Service::kWaitingStorage;
    clear();
    return Service::kFailed;
  }
  wipe(&exact,sizeof(exact));
  waiting_flash_=true;
  return Service::kWaitingFlash;
}

} // namespace orun_tlp

#endif
