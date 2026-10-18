#include "custody_ack_release_owner.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "product_record_store.h"
#include "security_store.h"

namespace orun_tlp {
namespace {
bool sameIdentity(const observation_store_format::RecordIdentity& left,
                  const observation_store_format::RecordIdentity& right) {
  return left.incarnation == right.incarnation &&
         left.sequence == right.sequence;
}
bool secureEqual32(const uint8_t* left,const uint8_t* right) {
  volatile uint8_t difference=0U;
  for(unsigned i=0U;i<32U;++i)
    difference=static_cast<uint8_t>(difference | (left[i]^right[i]));
  return difference==0U;
}
void secureZero(void* p,size_t count) {
  volatile uint8_t* bytes=static_cast<volatile uint8_t*>(p);
  while(count--!=0U)*bytes++=0U;
}
} // namespace

CustodyAckReleaseOwner::Submit
CustodyAckReleaseOwner::submitAuthenticated(
    const AuthenticatedCustodyAck& proof) {
  if(pending_) {
    ++diagnostics_.already_busy;
    return Submit::kBusy;
  }

  // A default-constructed or previously failed-auth token is INVALID.
  // No method exposed on the token allows an untrusted caller to make
  // verified_=true.
  if(!proof.verified_ || !proof.identity_.valid() ||
     !security_.custodyAckScopeMatches(
        proof.tracker_id_,proof.gateway_id_,proof.epoch_,
        proof.floor_,proof.generation_)) {
    ++diagnostics_.refused;
    return Submit::kRejected;
  }

  observation_store_control::ExactObject cached{};
  if(product_.findCachedExact(proof.identity_,cached)!=
          ObservationStore::ControlLookupResult::kFound ||
     !sameIdentity(cached.identity,proof.identity_) ||
     !product_.custodyCandidateMatchesRetainedExact(cached)) {
    ++diagnostics_.refused;
    return Submit::kRejected;
  }

  uint8_t digest[32]{};
  const bool digest_ok =
      custody_crypto::sha256(cached.object,cached.object_size,digest) &&
      secureEqual32(digest,proof.full_digest_);
  secureZero(digest,sizeof(digest));
  if(!digest_ok ||
     // Do not carry a stale authenticated privilege through another
     // owner's policy mutation, even if the record has not changed.
     !security_.custodyAckScopeMatches(
         proof.tracker_id_,proof.gateway_id_,proof.epoch_,
         proof.floor_,proof.generation_) ||
     !product_.custodyCandidateMatchesRetainedExact(cached) ||
     !product_.beginAuthenticatedCustodyRelease(proof.identity_)) {
    ++diagnostics_.refused;
    return Submit::kRejected;
  }

  pending_=true;
  pending_identity_=proof.identity_;
  ++diagnostics_.releases_started;
  return Submit::kStarted;
}

CustodyAckReleaseOwner::Service CustodyAckReleaseOwner::service() {
  if(!pending_)return Service::kIdle;
  bool durable=false;
  if(!product_.takeAuthenticatedCustodyReleaseResult(durable))
    return Service::kWaitingFlash;

  pending_=false;
  pending_identity_=observation_store_format::RecordIdentity{};
  if(durable) {
    ++diagnostics_.durable;
    return Service::kDurablyReleased;
  }
  ++diagnostics_.failed;
  return Service::kNotDurable;
}

} // namespace orun_tlp
