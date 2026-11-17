#include "tracker_critical_event_tx.h"

#include "monotonic_time.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {

bool TrackerCriticalEventTx::idleStorage() const {
  return records_.mounted() && !records_.preparing() &&
         !records_.pending() && records_.store().ready() &&
         !records_.store().faulted() && !records_.store().busy();
}

TrackerCriticalEventTx::Result TrackerCriticalEventTx::update(
    uint32_t now_ms, bool eligible) {
  if (phase_ == Phase::kBlocked) return Result::kBlocked;

  // Poll completion is independent of RF duty, just like the existing pilot.
  if (phase_ == Phase::kWaitingCache) {
    ProductRecordStore::CacheOutcome outcome{};
    if (!records_.takeCacheOutcome(outcome)) return Result::kNone;
    if (!outcome.durable || !sameSelected(outcome.identity)) return block();
    sealed_ = {};  // The committed exact object, not RAM, is RF authority.
    phase_ = Phase::kReady;
    return Result::kCacheDurable;
  }

  if (!idleStorage()) return Result::kNone;

  // Gateway custody ACK delivery is verified by CustodyAckReleaseOwner, not
  // this sender. Inspect only the actual durable OBS1 release state.
  if (phase_ == Phase::kWaitingAck || phase_ == Phase::kReady) {
    ObservationStore::Record record{};
    const auto lookup = records_.store().lookup(selected_, record);
    if (lookup == ObservationStore::LookupResult::kReadError)
      return Result::kReadError;
    if (lookup != ObservationStore::LookupResult::kFound ||
        record.handle.identity.incarnation != selected_.incarnation ||
        record.handle.identity.sequence != selected_.sequence ||
        record.kind != osf::RecordKind::kEvent ||
        record.release_uncertain || record.release_marker_exhausted)
      return block();
    if (record.released) {
      resetSelection();
      return Result::kNone;
    }
    if (phase_ == Phase::kWaitingAck) {
      if (!monotonic::reached(now_ms, next_attempt_ms_))
        return Result::kNone;
      phase_ = Phase::kReady;  // Resend SAME durable exact ciphertext.
    }
  }

  if (!eligible || !radio_.canSend()) return Result::kNone;
  if (!monotonic::reached(now_ms, next_attempt_ms_))
    return Result::kNone;

  if (phase_ == Phase::kSelect) {
    osf::RecordIdentity next{};
    if (!records_.store().peekNextIdentity(next) || !next.valid())
      return Result::kNone;
    if (scan_cursor_ == 0U || scan_incarnation_ != next.incarnation) {
      if (scan_incarnation_ == next.incarnation &&
          idle_next_ == next.sequence) return Result::kNone;
      ObservationStore::Record oldest{};
      const auto found = records_.oldestRetained(oldest);
      if (found == ObservationStore::LookupResult::kReadError)
        return Result::kReadError;
      scan_incarnation_ = next.incarnation;
      if (found == ObservationStore::LookupResult::kNone) {
        idle_next_ = next.sequence;
        return Result::kNone;
      }
      scan_cursor_ = oldest.handle.identity.sequence;
    }

    // EVENT priority is not permission to scan the entire Flash in one tick.
    for (uint8_t scanned = 0U;
         scanned < kScanBudget && scan_cursor_ < next.sequence;
         ++scanned, ++scan_cursor_) {
      osf::RecordIdentity id{};
      id.incarnation = next.incarnation;
      id.sequence = scan_cursor_;
      ObservationStore::Record record{};
      const auto status = records_.store().lookup(id, record);
      if (status == ObservationStore::LookupResult::kReadError)
        return Result::kReadError;
      if (status == ObservationStore::LookupResult::kNone)
        continue;
      if (record.handle.identity.incarnation != id.incarnation ||
          record.handle.identity.sequence != id.sequence)
        return block();
      if (record.kind != osf::RecordKind::kEvent || record.released)
        continue;
      if (record.release_uncertain || record.release_marker_exhausted ||
          record.schema != osf::kProductSchemaV1 ||
          record.payload_size != tlp::kEventPlaintextSize)
        return block();

      selected_ = id;
      ++scan_cursor_;
      phase_ = Phase::kNeedCache;
      break;
    }
    if (phase_ == Phase::kSelect) {
      if (scan_cursor_ >= next.sequence) {
        scan_cursor_ = 0U;
        idle_next_ = next.sequence;
      }
      return Result::kNone;
    }
  }

  if (phase_ == Phase::kNeedCache) {
    if (sealed_.object_size == 0U) {
      osc::ExactObject cached{};
      const auto existing = records_.findCachedExact(selected_, cached);
      if (existing == ObservationStore::ControlLookupResult::kReadError)
        return Result::kReadError;
      if (existing == ObservationStore::ControlLookupResult::kFound) {
        // Reboot or lost ACK: never re-seal with a different nonce.
        phase_ = Phase::kReady;
      } else {
        uint16_t occupied = 0U;
        if (!records_.store().exactObjectCount(occupied))
          return Result::kReadError;
        if (occupied >= osf::kMaxExactCustodyObjects) {
          next_attempt_ms_ = now_ms + kDeferredMs;
          return Result::kDeferred;
        }
        ObservationStore::Record record{};
        if (records_.store().lookup(selected_, record) !=
                ObservationStore::LookupResult::kFound ||
            record.kind != osf::RecordKind::kEvent || record.released ||
            record.release_uncertain ||
            record.payload_size != tlp::kEventPlaintextSize)
          return block();
        tlp::EventPlaintext event{};
        if (tlp::decodeEventPlaintext(
                record.payload, record.payload_size, &event) !=
                tlp::ProductSecureCodecResult::kOk ||
            event.record_incarnation != selected_.incarnation ||
            event.record_sequence != selected_.sequence)
          return block();

        uint8_t wire[tlp::kEventFrameSize]{};
        const auto sealed = crypto_.protectNextEvent(
            tlp::kProductPathFlagRelayAllowed |
                tlp::kProductPathFlagCustodyRequested,
            event, wire);
        if (sealed == ProductSecureCryptoResult::kUnavailable) {
          next_attempt_ms_ = now_ms + kDeferredMs;
          return Result::kDeferred;
        }
        if (sealed != ProductSecureCryptoResult::kOk) return block();
        sealed_.identity = selected_;
        sealed_.record_kind = osf::RecordKind::kEvent;
        sealed_.object_size = static_cast<uint8_t>(sizeof(wire));
        for (size_t i = 0U; i < sizeof(wire); ++i) {
          sealed_.object[i] = wire[i];
          wire[i] = 0U;
        }
      }
    }
    if (phase_ == Phase::kNeedCache) {
      const auto admission = records_.submitExactCache(sealed_);
      if (admission == ProductRecordStore::CacheSubmit::kBusy)
        return Result::kNone;
      if (admission == ProductRecordStore::CacheSubmit::kStarted) {
        phase_ = Phase::kWaitingCache;
        return Result::kCachePending;
      }
      if (admission == ProductRecordStore::CacheSubmit::kAlreadyDurable) {
        sealed_ = {};
        phase_ = Phase::kReady;
      } else if (admission == ProductRecordStore::CacheSubmit::kNoCapacity) {
        next_attempt_ms_ = now_ms + kDeferredMs;
        return Result::kDeferred;
      } else {
        return block();  // Unknown Flash/cache evidence: fail closed.
      }
    }
  }

  if (phase_ != Phase::kReady) return Result::kNone;

  osc::ExactObject exact{};
  if (records_.findCachedExact(selected_, exact) !=
          ObservationStore::ControlLookupResult::kFound ||
      exact.record_kind != osf::RecordKind::kEvent ||
      exact.object_size != tlp::kEventFrameSize ||
      !records_.custodyCandidateMatchesRetainedExact(exact))
    return block();

  tlp::ProductSecureFrame header{};
  if (tlp::decodeProductSecureFrame(
          exact.object, exact.object_size, &header) !=
          tlp::ProductSecureCodecResult::kOk ||
      header.device_id != radio_.deviceId() ||
      header.app_family != tlp::kProductAppFamilyEvent ||
      (header.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U ||
      (header.path_flags & tlp::kProductPathFlagRelayAllowed) == 0U)
    return block();

  if (!radio_.sendProductSecurePacket(exact.object, exact.object_size)) {
    next_attempt_ms_ = now_ms + kDeferredMs;
    return Result::kDeferred;
  }
  phase_ = Phase::kWaitingAck;
  next_attempt_ms_ = now_ms + kRetryMs;
  return Result::kSent;  // Started RF, not Gateway or Backend delivery.
}

}  // namespace orun_tlp
