#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
#include "tracker_critical_event_tx.h"

#include "monotonic_time.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {

bool TrackerCriticalEventTx::idleStorage() const {
  return records_.mounted() && !records_.preparing() &&
         !records_.pending() && records_.store().ready() &&
         !records_.store().faulted() && !records_.store().busy();
}

namespace {
bool sameId(const osf::RecordIdentity& a, const osf::RecordIdentity& b) {
  return a.incarnation == b.incarnation && a.sequence == b.sequence;
}
}

bool TrackerCriticalEventTx::awaitingReceipt(const osf::RecordIdentity& id, uint32_t now) const {
  for (const auto& tx : transmitted_)
    if (sameId(tx.id, id) && !monotonic::elapsed(now, tx.sent_ms, kReceiptWaitMs)) return true;
  return false;
}

void TrackerCriticalEventTx::rememberTransmission(uint32_t now) {
  for (auto& tx : transmitted_) {
    if (sameId(tx.id, selected_)) { tx.sent_ms = now; return; }
  }
  for (auto& tx : transmitted_) {
    // Every active entry corresponds to a durable exact slot. A new slot
    // can reuse tracking space from a completed, expired or reclaimed one.
    ObservationStore::Record old{};
    bool reusable = !tx.id.valid() || monotonic::elapsed(now, tx.sent_ms, kReceiptWaitMs);
    if (!reusable) {
      const auto found = records_.store().lookup(tx.id, old);
      reusable = found == ObservationStore::LookupResult::kNone ||
                 (found == ObservationStore::LookupResult::kFound && old.released);
    }
    if (reusable) {
      tx.id = selected_;
      tx.sent_ms = now;
      return;
    }
  }
}

TrackerCriticalEventTx::Result TrackerCriticalEventTx::selectFresh(uint32_t now) {
  osf::RecordIdentity next{};
  if (!records_.store().peekNextIdentity(next) || !next.valid()) return Result::kReadError;
  if (!sameId(next, observed_next_)) {
    observed_next_ = next;
    fresh_cursor_ = next.sequence - 1U;
  }
  for (unsigned n = 0; n < kScanBudget && fresh_cursor_; ++n) {
    osf::RecordIdentity id = next;
    id.sequence = fresh_cursor_;
    ObservationStore::Record record{};
    const auto found = records_.store().lookup(id, record);
    if (found == ObservationStore::LookupResult::kReadError) return Result::kReadError;
    --fresh_cursor_;
    if (found == ObservationStore::LookupResult::kNone || record.kind != osf::RecordKind::kPeriodic)
      continue;
    fresh_cursor_ = 0;
    if (!sameId(record.handle.identity, id) || record.release_uncertain ||
        record.release_marker_exhausted) return block();
    if (record.released || sameId(last_fresh_, id) || awaitingReceipt(id, now)) break;
    if (!sameSelected(id) || phase_ == Phase::kSelect) {
      resetSelection();
      selected_ = id;
      phase_ = Phase::kNeedCache;
    }
    fresh_ = true;
    break;
  }
  return Result::kNone;
}

TrackerCriticalEventTx::Result TrackerCriticalEventTx::update(
    uint32_t now_ms, bool eligible) {
  const bool history = historyMode();
  // Report a hard fault only on the transition to blocked; do not flood
  // the serial console with an error on every cooperative loop iteration.
  if (phase_ == Phase::kBlocked) return Result::kNone;

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
  if (history && eligible) {
    const auto result = selectFresh(now_ms);
    if (result != Result::kNone || fresh_cursor_) return result;
  }

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
        record.kind != (history ? osf::RecordKind::kPeriodic :
                                   osf::RecordKind::kEvent) ||
        record.release_uncertain || record.release_marker_exhausted)
      return block();
    if (record.released) {
      // Authenticated ACK owner committed release: next critical occurrence
      // may be sent immediately, without waiting for the old ACK timeout.
      resetSelection();
      return Result::kNone;
    }
    if (phase_ == Phase::kWaitingAck) {
      if (deadline_armed_ && !monotonic::reached(now_ms, next_attempt_ms_))
        return Result::kNone;
      phase_ = Phase::kReady;  // Resend SAME durable exact ciphertext.
    }
  }

  if (!eligible || !radio_.canSend()) return Result::kNone;
  if (deadline_armed_ && !monotonic::reached(now_ms, next_attempt_ms_))
    return Result::kNone;

  if (phase_ == Phase::kSelect) {
    osf::RecordIdentity next{};
    if (!records_.store().peekNextIdentity(next) || !next.valid())
      return Result::kNone;
    if (scan_cursor_ == 0U || scan_incarnation_ != next.incarnation) {
      if (!history && scan_incarnation_ == next.incarnation &&
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
      if (record.kind != (history ? osf::RecordKind::kPeriodic :
                                    osf::RecordKind::kEvent) ||
          record.released || (history && awaitingReceipt(id, now_ms)))
        continue;
      if (record.release_uncertain || record.release_marker_exhausted ||
          record.schema != osf::kProductSchemaV1 ||
          record.payload_size != (history ? tlp::kPeriodicPlaintextSize :
                                           tlp::kEventPlaintextSize))
        return block();

      fresh_ = false;
      selected_ = id;
      ++scan_cursor_;
      phase_ = Phase::kNeedCache;
      break;
    }
    if (phase_ == Phase::kSelect) {
      if (scan_cursor_ >= next.sequence) {
        scan_cursor_ = 0U;
        idle_next_ = next.sequence;
        // Periodic HISTORY may resweep retained exact objects after a
        // cooldown, including when a Gateway ACK was lost.
        if (history) {
          deadline_armed_ = true;
          next_attempt_ms_ = now_ms + kHistorySpacingMs;
        }
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
        // HISTORY never fills the final exact slot: it is reserved for
        // a critical EVENT. A full/no-ACK history cache cannot gate LIVE.
        const uint16_t ceiling = history ? kHistoryCacheCeiling :
                                           osf::kMaxExactCustodyObjects;
        if (occupied >= ceiling && fresh_) {
          // During an extended outage preserve current visibility. The
          // measurement stays in OBS1 until an exact retry can be admitted.
          ObservationStore::Record record{};
          tlp::PeriodicObservationPlaintext plain{};
          if (records_.store().lookup(selected_, record) != ObservationStore::LookupResult::kFound ||
              record.released || record.release_uncertain || record.release_marker_exhausted ||
              tlp::decodePeriodicPlaintext(record.payload, record.payload_size, &plain) !=
                  tlp::ProductSecureCodecResult::kOk || plain.record_sequence != selected_.sequence ||
              plain.record_incarnation != selected_.incarnation) return Result::kReadError;
          uint8_t wire[tlp::kPeriodicFrameSize]{};
          if (crypto_.protectNextPeriodic(0U, plain, wire) != ProductSecureCryptoResult::kOk ||
              !radio_.sendProductSecurePacket(wire, sizeof(wire))) {
            next_attempt_ms_ = now_ms + kDeferredMs;
            deadline_armed_ = true;
            return Result::kDeferred;
          }
          last_fresh_ = selected_;
          fresh_ = false;
          resetSelection();
          return Result::kSent;
        }
        if (occupied >= ceiling) {
          if (history) {
            // No free HISTORY slot. Reuse an existing EXACT cached PERIODIC
            // object without consuming a nonce or evicting a record.
            osc::ExactObject cached[osf::kMaxExactCustodyObjects]{};
            uint16_t count = 0U;
            if (records_.snapshotCustodyCandidates(cached, count)) {
              const osc::ExactObject* oldest = nullptr;
              const osc::ExactObject* after_retry = nullptr;
              const uint64_t incarnation = records_.store().incarnation();
              for (uint16_t i = 0U; i < count; ++i) {
                if (cached[i].record_kind != osf::RecordKind::kPeriodic ||
                    cached[i].identity.incarnation != incarnation ||
                    awaitingReceipt(cached[i].identity, now_ms)) continue;
                const uint32_t seq = cached[i].identity.sequence;
                if (oldest == nullptr ||
                    seq < oldest->identity.sequence)
                  oldest = &cached[i];
                if (seq > last_history_retry_seq_ &&
                    (after_retry == nullptr ||
                     seq < after_retry->identity.sequence))
                  after_retry = &cached[i];
              }
              const osc::ExactObject* retry =
                  after_retry != nullptr ? after_retry : oldest;
              if (retry != nullptr) {
                selected_ = retry->identity;
                last_history_retry_seq_ = selected_.sequence;
                phase_ = Phase::kReady;
              }
            }
          }
          if (phase_ != Phase::kReady) {
            next_attempt_ms_ = now_ms + kDeferredMs;
            deadline_armed_ = true;
            return Result::kDeferred;
          }
        }
        if (phase_ == Phase::kNeedCache) {
          ObservationStore::Record record{};
          if (records_.store().lookup(selected_, record) !=
                  ObservationStore::LookupResult::kFound ||
              record.kind != (history ? osf::RecordKind::kPeriodic :
                                       osf::RecordKind::kEvent) ||
              record.released || record.release_uncertain ||
              record.payload_size != (history ? tlp::kPeriodicPlaintextSize :
                                               tlp::kEventPlaintextSize))
            return block();

          const uint8_t flags = tlp::kProductPathFlagRelayAllowed |
                                tlp::kProductPathFlagCustodyRequested;
          if (history) {
            tlp::PeriodicObservationPlaintext periodic{};
            if (tlp::decodePeriodicPlaintext(
                    record.payload, record.payload_size, &periodic) !=
                    tlp::ProductSecureCodecResult::kOk ||
                periodic.record_incarnation != selected_.incarnation ||
                periodic.record_sequence != selected_.sequence)
              return block();
            uint8_t wire[tlp::kPeriodicFrameSize]{};
            const auto protected_result =
                crypto_.protectNextPeriodic(flags, periodic, wire);
            if (protected_result == ProductSecureCryptoResult::kUnavailable) {
              next_attempt_ms_ = now_ms + kDeferredMs;
              deadline_armed_ = true;
              return Result::kDeferred;
            }
            if (protected_result != ProductSecureCryptoResult::kOk)
              return block();
            sealed_.identity = selected_;
            sealed_.record_kind = osf::RecordKind::kPeriodic;
            sealed_.object_size = static_cast<uint8_t>(sizeof(wire));
            for (size_t i = 0U; i < sizeof(wire); ++i) {
              sealed_.object[i] = wire[i];
              wire[i] = 0U;
            }
          } else {
            tlp::EventPlaintext event{};
            if (tlp::decodeEventPlaintext(
                    record.payload, record.payload_size, &event) !=
                    tlp::ProductSecureCodecResult::kOk ||
                event.record_incarnation != selected_.incarnation ||
                event.record_sequence != selected_.sequence)
              return block();

            uint8_t wire[tlp::kEventFrameSize]{};
            const auto protected_result =
                crypto_.protectNextEvent(flags, event, wire);
            if (protected_result == ProductSecureCryptoResult::kUnavailable) {
              next_attempt_ms_ = now_ms + kDeferredMs;
              deadline_armed_ = true;
              return Result::kDeferred;
            }
            if (protected_result != ProductSecureCryptoResult::kOk)
              return block();
            sealed_.identity = selected_;
            sealed_.record_kind = osf::RecordKind::kEvent;
            sealed_.object_size = static_cast<uint8_t>(sizeof(wire));
            for (size_t i = 0U; i < sizeof(wire); ++i) {
              sealed_.object[i] = wire[i];
              wire[i] = 0U;
            }
          }
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
        deadline_armed_ = true;
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
      exact.record_kind != (history ? osf::RecordKind::kPeriodic :
                                     osf::RecordKind::kEvent) ||
      exact.object_size != (history ? tlp::kPeriodicFrameSize :
                                     tlp::kEventFrameSize) ||
      !records_.custodyCandidateMatchesRetainedExact(exact))
    return block();

  tlp::ProductSecureFrame header{};
  if (tlp::decodeProductSecureFrame(
          exact.object, exact.object_size, &header) !=
          tlp::ProductSecureCodecResult::kOk ||
      header.device_id != radio_.deviceId() ||
      header.app_family != (history ? tlp::kProductAppFamilyPeriodic :
                                      tlp::kProductAppFamilyEvent) ||
      (header.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U ||
      (header.path_flags & tlp::kProductPathFlagRelayAllowed) == 0U)
    return block();

  if (!radio_.sendProductSecurePacket(exact.object, exact.object_size)) {
    next_attempt_ms_ = now_ms + kDeferredMs;
    deadline_armed_ = true;
    return Result::kDeferred;
  }
  // The EVENT owner waits for its one exact ACK before selecting the next
  // EVENT. Routine HISTORY walks to other retained records while the
  // Gateway may batch 1..5 exact packets; it never counts TX as release.
  if (history) {
    rememberTransmission(now_ms);
    if (fresh_) last_fresh_ = selected_;
    fresh_ = false;
  }
  phase_ = history ? Phase::kSelect : Phase::kWaitingAck;
  next_attempt_ms_ = now_ms +
      (history ? kHistorySpacingMs : kRetryMs);
  deadline_armed_ = true;
  return Result::kSent;  // Started RF, not Gateway or Backend delivery.
}

}  // namespace orun_tlp

#endif
