#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
#include "tracker_ackless_live_tx.h"

#include "monotonic_time.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {

TrackerAcklessLiveTx::Event TrackerAcklessLiveTx::update(
    uint32_t now_ms, bool eligible) {
  if (!eligible || !records_.mounted() || records_.preparing() ||
      records_.pending() || !records_.store().ready() ||
      records_.store().faulted() || records_.store().busy() ||
      !radio_.canSend()) return Event::kNone;
  if (throttled_ && !monotonic::reached(now_ms, next_try_ms_))
    return Event::kNone;
  throttled_ = false;

  osf::RecordIdentity next{};
  if (!records_.store().peekNextIdentity(next) || !next.valid() ||
      next.sequence <= 1U) return Event::kNone;

  // EVENT and PERIODIC deliberately use the SAME durable sequence allocator.
  // The newest record may be an EVENT. Do not treat it as a malformed LIVE
  // packet or stop later PERIODIC observations from being sent. Keep the scan
  // bounded per loop: many consecutive events must not monopolize Flash reads.
  if (live_search_incarnation_ != next.incarnation ||
      live_search_next_seq_ != next.sequence) {
    live_search_incarnation_ = next.incarnation;
    live_search_next_seq_ = next.sequence;
    live_search_cursor_ = next.sequence - 1U;
  }

  for (uint8_t scanned = 0U;
       scanned < kLiveLookupBudget && live_search_cursor_ != 0U;
       ++scanned) {
    osf::RecordIdentity candidate{};
    candidate.incarnation = next.incarnation;
    candidate.sequence = live_search_cursor_;
    ObservationStore::Record record{};
    const auto lookup = records_.store().lookup(candidate, record);
    if (lookup == ObservationStore::LookupResult::kReadError) {
      defer(now_ms);
      return Event::kReadError;
    }
    if (lookup == ObservationStore::LookupResult::kNone) {
      --live_search_cursor_;
      continue;
    }
    if (!sameIdentity(record.handle.identity, candidate) ||
        record.release_uncertain || record.release_marker_exhausted) {
      defer(now_ms);
      return Event::kBadRecord;
    }
    if (record.kind != osf::RecordKind::kPeriodic) {
      // The EVENT sender owns EVENT transmission and its receipt semantics.
      // Skipping an EVENT here never releases or acknowledges it.
      --live_search_cursor_;
      continue;
    }
    if (record.released) {
      // Do not resurrect an older PERIODIC as current LIVE just because
      // the latest PERIODIC has already been durably delivered.
      live_search_cursor_ = 0U;
      return Event::kNone;
    }
    if (record.schema != osf::kProductSchemaV1 ||
        record.payload_size != tlp::kPeriodicPlaintextSize) {
      defer(now_ms);
      return Event::kBadRecord;
    }
    if (sameIdentity(candidate, last_sent_)) {
      // Already offered the latest retained PERIODIC in this boot. HISTORY
      // can retry it separately; an EVENT append does not re-arm LIVE.
      live_search_cursor_ = 0U;
      return Event::kNone;
    }

    tlp::PeriodicObservationPlaintext plaintext{};
    if (tlp::decodePeriodicPlaintext(
            record.payload, record.payload_size, &plaintext) !=
            tlp::ProductSecureCodecResult::kOk ||
        plaintext.record_incarnation != candidate.incarnation ||
        plaintext.record_sequence != candidate.sequence) {
      defer(now_ms);
      return Event::kBadRecord;
    }

    // Direct-only LIVE, not custody requested. A fresh never-reused AEAD TX
    // nonce is reserved by the crypto owner, with no exact cache slot/ACK.
    uint8_t wire[tlp::kPeriodicFrameSize]{};
    const auto crypto = crypto_.protectNextPeriodic(0U, plaintext, wire);
    if (crypto != ProductSecureCryptoResult::kOk) {
      for (size_t i = 0U; i < sizeof(wire); ++i) wire[i] = 0U;
      defer(now_ms);
      return crypto == ProductSecureCryptoResult::kUnavailable
                 ? Event::kDeferred : Event::kCryptoError;
    }
    const bool started = radio_.sendProductSecurePacket(wire, sizeof(wire));
    for (size_t i = 0U; i < sizeof(wire); ++i) wire[i] = 0U;
    if (!started) {
      // Preserve the candidate for retry. Starting a TX is NOT delivery.
      defer(now_ms);
      return Event::kDeferred;
    }

    last_sent_ = candidate;
    live_search_cursor_ = 0U;
    if (sent_count_ != UINT32_MAX) ++sent_count_;
    return Event::kSent;
  }
  // Scan resumes on the next loop without losing latest-first priority.
  return Event::kNone;
}


}  // namespace orun_tlp
#endif
