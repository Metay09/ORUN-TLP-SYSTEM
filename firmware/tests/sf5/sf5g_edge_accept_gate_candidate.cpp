#include "sf5g_edge_accept_gate_candidate.h"

namespace orun_tlp {
namespace sf5g_edge_accept_candidate {
namespace {
bool exactBytes(const uint8_t* a, const uint8_t* b, size_t size) {
  if (a == nullptr || b == nullptr) return false;
  uint8_t different = 0U;
  for (size_t i = 0U; i < size; ++i)
    different |= static_cast<uint8_t>(a[i] ^ b[i]);
  return different == 0U;
}
bool validAttempt(const Attempt& p, uint16_t pages) {
  return pages > 0U && pages <= page::kMaxRegionPages &&
         p.gateway_instance != 0U && p.attempt_id != 0U &&
         p.edge_issuer != 0U && p.source_page < pages &&
         p.source_slot < page::kRecordsPerPage &&
         p.source_generation != 0U && p.source_generation != UINT64_MAX &&
         rec::productObjectShape(p.object, p.object_size);
}
bool exactBinding(const Attempt& p, const VerifiedClaim& v) {
  return v.gateway_instance == p.gateway_instance &&
         v.attempt_id == p.attempt_id &&
         v.edge_issuer == p.edge_issuer &&
         v.source_page == p.source_page &&
         v.source_slot == p.source_slot &&
         v.source_generation == p.source_generation &&
         v.object_size == p.object_size &&
         v.object_size <= rec::kObjectMax &&
         v.key_epoch != 0U && v.key_epoch != UINT32_MAX &&
         v.security_counter != 0U &&
         v.security_counter != UINT64_MAX &&
         v.scope == DurableScope::kEdgeDurableCommitted &&
         exactBytes(p.object, v.object, p.object_size);
}
}  // namespace

GateResult evaluate(const Attempt& pending, uint16_t actual_page_count,
                    const uint8_t* raw_receipt, size_t receipt_size,
                    const ReceiptVerifier& verifier, ReplayStore& replay) {
  if (!validAttempt(pending, actual_page_count) ||
      raw_receipt == nullptr || receipt_size == 0U)
    return GateResult::kInvalidAttempt;
  // A late Edge answer must not cancel/rewrite an unknown local flash write.
  if (pending.local_fallback_started)
    return GateResult::kFallbackInProgress;

  VerifiedClaim claim{};
  const VerifyStatus checked =
      verifier.verify(raw_receipt, receipt_size, claim);
  if (checked == VerifyStatus::kRejected)
    return GateResult::kAuthRejected;
  if (checked != VerifyStatus::kVerified)
    return GateResult::kAuthUnavailable;
  if (!exactBinding(pending, claim))
    return GateResult::kBindingMismatch;

  // CRITICAL: the replay adapter must report *persisted* reservation, not
  // started/enqueued/RAM-only. A failed/uncertain/duplicate result has no
  // effect on custody responsibility.
  const ReplayStatus admitted =
      replay.admitDurably(claim.edge_issuer, claim.key_epoch,
                          claim.security_counter);
  if (admitted == ReplayStatus::kReplay)
    return GateResult::kReplayRejected;
  if (admitted != ReplayStatus::kDurablyAccepted)
    return GateResult::kReplayUnavailable;

  // Not proof of cryptographic authenticity in this host mock.
  // No code here produces an ACK, a flash erase token or a custody release.
  return GateResult::kCandidateOnly;
}

}  // namespace sf5g_edge_accept_candidate
}  // namespace orun_tlp
