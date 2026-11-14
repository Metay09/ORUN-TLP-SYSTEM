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

  osf::RecordIdentity newest = next;
  --newest.sequence;
  if (sameIdentity(newest, last_sent_)) return Event::kNone;

  ObservationStore::Record record{};
  const auto lookup = records_.store().lookup(newest, record);
  if (lookup == ObservationStore::LookupResult::kReadError) {
    defer(now_ms);
    return Event::kReadError;
  }
  if (lookup != ObservationStore::LookupResult::kFound ||
      !sameIdentity(record.handle.identity, newest) || record.released ||
      record.release_uncertain || record.release_marker_exhausted ||
      record.kind != osf::RecordKind::kPeriodic ||
      record.schema != osf::kProductSchemaV1 ||
      record.payload_size != tlp::kPeriodicPlaintextSize) {
    defer(now_ms);
    return Event::kBadRecord;
  }

  tlp::PeriodicObservationPlaintext plaintext{};
  if (tlp::decodePeriodicPlaintext(
          record.payload, record.payload_size, &plaintext) !=
          tlp::ProductSecureCodecResult::kOk ||
      plaintext.record_incarnation != newest.incarnation ||
      plaintext.record_sequence != newest.sequence) {
    defer(now_ms);
    return Event::kBadRecord;
  }

  // Direct-only LIVE, not custody requested. The PRODUCT_SECURE AEAD owner
  // reserves a fresh never-reused TX nonce. It does not mutate record Flash or
  // allocate one of the four exact custody objects.
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
    // A duty denial or driver refusal is NOT a receipt, a TX, or a released
    // record. Avoid consuming another fresh AEAD nonce on every loop pass.
    defer(now_ms);
    return Event::kDeferred;
  }

  last_sent_ = newest;
  last_live_sent_at_ms_ = now_ms;
  if (sent_count_ != UINT32_MAX) ++sent_count_;
  return Event::kSent;
}


TrackerAcklessLiveTx::Event TrackerAcklessLiveTx::updateHistory(
    uint32_t now_ms, bool eligible) {
  // This is a separate best-effort, noncustody HISTORY path, never a release
  // authority. A caller must offer LIVE first and enforce its shared RF budget.
  if (!eligible || !records_.mounted() || records_.preparing() ||
      records_.pending() || !records_.store().ready() ||
      records_.store().faulted() || records_.store().busy() ||
      !radio_.canSend()) return Event::kNone;

  if (!monotonic::reached(now_ms, next_history_try_ms_))
    return Event::kNone;
  if (history_paused_) {
    history_paused_ = false;
    history_cursor_ = 0U;
  }

  osf::RecordIdentity next{};
  if (!records_.store().peekNextIdentity(next) || !next.valid() ||
      next.sequence <= 1U) return Event::kNone;
  if (history_cursor_ == 0U || history_incarnation_ != next.incarnation) {
    ObservationStore::Record oldest{};
    const auto result = records_.oldestRetained(oldest);
    if (result == ObservationStore::LookupResult::kReadError) {
      next_history_try_ms_ = now_ms + kRetryDelayMs;
      return Event::kReadError;
    }
    if (result == ObservationStore::LookupResult::kNone) {
      history_paused_ = true;
      next_history_try_ms_ = now_ms + kHistorySweepPauseMs;
      return Event::kNone;
    }
    history_incarnation_ = next.incarnation;
    history_cursor_ = oldest.handle.identity.sequence;
  }

  // A bounded scan keeps sparse/released records from monopolizing the loop.
  for (unsigned scan = 0U; scan < 8U; ++scan) {
    if (history_cursor_ == 0U || history_cursor_ >= next.sequence) {
      history_paused_ = true;
      next_history_try_ms_ = now_ms + kHistorySweepPauseMs;
      return Event::kNone;
    }
    const uint32_t candidate = history_cursor_;
    osf::RecordIdentity id{};
    id.incarnation = history_incarnation_;
    id.sequence = candidate;
    // Avoid an immediate duplicate of fresh LIVE, but never suppress that
    // identity forever. An unanswered hourly LIVE needs history retry too.
    if (sameIdentity(id, last_sent_) &&
        !monotonic::reached(
            now_ms, last_live_sent_at_ms_ + kHistorySpacingMs)) {
      next_history_try_ms_ = last_live_sent_at_ms_ + kHistorySpacingMs;
      return Event::kNone;
    }
    ObservationStore::Record record{};
    const auto lookup = records_.store().lookup(id, record);
    if (lookup == ObservationStore::LookupResult::kReadError) {
      next_history_try_ms_ = now_ms + kRetryDelayMs;
      return Event::kReadError;
    }
    if (lookup == ObservationStore::LookupResult::kNone || record.released) {
      ++history_cursor_;
      continue;
    }
    // EVENT is never silently acknowledged. Its secure transport has a
    // different schema; a later EVENT sender must explicitly own that path.
    if (!sameIdentity(record.handle.identity, id) ||
        record.release_uncertain || record.release_marker_exhausted ||
        record.kind != osf::RecordKind::kPeriodic ||
        record.schema != osf::kProductSchemaV1 ||
        record.payload_size != tlp::kPeriodicPlaintextSize) {
      ++history_cursor_;
      return Event::kBadRecord;
    }
    tlp::PeriodicObservationPlaintext plaintext{};
    if (tlp::decodePeriodicPlaintext(
            record.payload, record.payload_size, &plaintext) !=
            tlp::ProductSecureCodecResult::kOk ||
        plaintext.record_incarnation != id.incarnation ||
        plaintext.record_sequence != id.sequence) {
      ++history_cursor_;
      return Event::kBadRecord;
    }

    // Use the same nonce-reserving AEAD and noncustody wire as LIVE. Never
    // allocate a legacy exact-cache slot or infer an ACK from RF success.
    uint8_t wire[tlp::kPeriodicFrameSize]{};
    const auto crypt = crypto_.protectNextPeriodic(0U, plaintext, wire);
    if (crypt != ProductSecureCryptoResult::kOk) {
      for (size_t i = 0U; i < sizeof(wire); ++i) wire[i] = 0U;
      next_history_try_ms_ = now_ms + kRetryDelayMs;
      return crypt == ProductSecureCryptoResult::kUnavailable
                 ? Event::kDeferred : Event::kCryptoError;
    }
    const bool started = radio_.sendProductSecurePacket(wire, sizeof(wire));
    for (size_t i = 0U; i < sizeof(wire); ++i) wire[i] = 0U;
    if (!started) {
      next_history_try_ms_ = now_ms + kRetryDelayMs;
      return Event::kDeferred;
    }
    last_history_sent_ = id;
    if (history_sent_count_ != UINT32_MAX) ++history_sent_count_;
    ++history_cursor_;
    next_history_try_ms_ = now_ms + kHistorySpacingMs;
    return Event::kHistorySent;
  }
  return Event::kNone;
}

}  // namespace orun_tlp
