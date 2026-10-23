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

bool sameDigest(const uint8_t* left, const uint8_t* right, size_t count) {
  volatile uint8_t diff = 0U;
  for (size_t i = 0U; i < count; ++i)
    diff = static_cast<uint8_t>(diff | (left[i] ^ right[i]));
  return diff == 0U;
}
void wipe(void* memory, size_t size) {
  volatile uint8_t* data = static_cast<volatile uint8_t*>(memory);
  while (size-- != 0U) *data++ = 0U;
}

} // namespace

CustodyAckReleaseOwner::Submit CustodyAckReleaseOwner::submit(
    const uint8_t* ack, size_t length) {
  if (pending_) return Submit::kBusy;

  AuthenticatedCustodyAck proof{};
  if (verifier_.verifyFromStore(ack,length,product_,proof) !=
          CustodyAckVerifyResult::kAuthenticated ||
      !proof.verified_ || !proof.identity_.valid())
    return Submit::kAuthenticationRejected;

  // Policy and exact cached object must remain current at the point of
  // handing the flash mutation to the sole durable release owner.
  if (!security_.custodyAckScopeMatches(
          proof.tracker_id_,proof.gateway_id_,proof.key_epoch_,
          proof.floor_,proof.generation_))
    return Submit::kAuthenticationRejected;

  observation_store_control::ExactObject objects[
      observation_store_format::kMaxExactCustodyObjects]{};
  uint16_t count = 0U;
  if (!product_.snapshotCustodyCandidates(objects,count))
    return Submit::kStorageUnavailable;

  uint16_t matching = 0U;
  observation_store_control::ExactObject selected{};
  for (uint16_t i = 0U; i < count; ++i) {
    if (!sameIdentity(objects[i].identity,proof.identity_)) continue;
    if (objects[i].object_size == 0U ||
        objects[i].object_size > sizeof(objects[i].object))
      return Submit::kStorageUnavailable;
    uint8_t digest[32]{};
    const bool calculated = custody_crypto::sha256(
        objects[i].object,objects[i].object_size,digest);
    const bool matches = calculated &&
        sameDigest(digest,proof.full_digest_,sizeof(digest));
    wipe(digest,sizeof(digest));
    if (!matches) return Submit::kAuthenticationRejected;
    ++matching;
    selected=objects[i];
  }
  if (matching != 1U ||
      !product_.requestAuthenticatedCustodyRelease(selected))
    return Submit::kStorageUnavailable;

  pending_identity_ = proof.identity_;
  pending_ = true;
  return Submit::kStarted;
}

CustodyAckReleaseOwner::Service CustodyAckReleaseOwner::service() {
  if (!pending_) return Service::kIdle;
  observation_store_format::RecordIdentity identity{};
  bool durable=false;
  if (!product_.takeCustodyReleaseOutcome(identity,durable))
    return Service::kWaitingFlash;

  const bool exact = sameIdentity(identity,pending_identity_);
  pending_identity_ = observation_store_format::RecordIdentity{};
  pending_=false;
  return (exact && durable) ? Service::kDurablyReleased : Service::kFailed;
}

} // namespace orun_tlp
