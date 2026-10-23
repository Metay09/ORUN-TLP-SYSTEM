#include "custody_ack_verifier.h"

#include "product_record_store.h"

namespace orun_tlp {

CustodyAckVerifyResult CustodyAckVerifier::verifyFromStore(
    const uint8_t* ack, size_t length,
    const ProductRecordStore& product_store,
    AuthenticatedCustodyAck& authenticated) {
  observation_store_control::ExactObject objects[
      observation_store_format::kMaxExactCustodyObjects]{};
  uint16_t count=0U;
  if (!product_store.snapshotCustodyCandidates(objects,count))
    return CustodyAckVerifyResult::kStoreUnavailable;

  // Verify against a local bounded snapshot, but publish nothing
  // until the real durable Store still matches that exact object.
  AuthenticatedCustodyAck pending{};
  uint16_t selected=UINT16_MAX;
  const CustodyAckVerifyResult verdict=
      verifyCandidates(ack,length,objects,count,pending,selected);
  if (verdict!=CustodyAckVerifyResult::kAuthenticated)
    return verdict;
  if (selected>=count ||
      !product_store.custodyCandidateMatchesRetainedExact(objects[selected]))
    return CustodyAckVerifyResult::kStoreUnavailable;

  authenticated=pending;
  return CustodyAckVerifyResult::kAuthenticated;
}

} // namespace orun_tlp
