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
  if (sent_count_ != UINT32_MAX) ++sent_count_;
  return Event::kSent;
}

}  // namespace orun_tlp
