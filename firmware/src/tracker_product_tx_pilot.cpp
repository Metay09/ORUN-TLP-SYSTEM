#include "tracker_product_tx_pilot.h"

namespace orun_tlp {

bool TrackerProductTxPilot::sameIdentity(
    const osf::RecordIdentity& other) const {
  return other.incarnation == identity_.incarnation &&
         other.sequence == identity_.sequence;
}

bool TrackerProductTxPilot::storageIdle() const {
  return records_.mounted() && !records_.preparing() &&
         !records_.pending() && records_.store().ready() &&
         !records_.store().faulted() && !records_.store().busy();
}

namespace {
bool usablePeriodic(const ObservationStore::Record& record) {
  return record.handle.identity.valid() && !record.released &&
         !record.release_uncertain && !record.release_marker_exhausted &&
         record.kind == osf::RecordKind::kPeriodic &&
         record.schema == osf::kProductSchemaV1 &&
         record.payload_size == tlp::kPeriodicPlaintextSize;
}
}  // namespace

bool TrackerProductTxPilot::selectLive(ObservationStore::Record& record) const {
  osf::RecordIdentity next{};
  if (!records_.store().peekNextIdentity(next) || next.sequence <= 1U)
    return false;
  osf::RecordIdentity newest{};
  newest.incarnation = next.incarnation;
  newest.sequence = next.sequence - 1U;
  ObservationStore::Record candidate{};
  if (records_.store().lookup(newest, candidate) !=
          ObservationStore::LookupResult::kFound ||
      !usablePeriodic(candidate))
    return false;
  // Only take it if it is already cached or a cache slot is free; never
  // evict another custody object to make room for live data.
  osc::ExactObject cached{};
  if (records_.findCachedExact(candidate.handle.identity, cached) !=
      ObservationStore::ControlLookupResult::kFound) {
    uint16_t occupied = 0U;
    if (!records_.store().exactObjectCount(occupied) ||
        occupied >= osf::kMaxExactCustodyObjects)
      return false;
  }
  record = candidate;
  return true;
}

bool TrackerProductTxPilot::selectCachedFallback(
    osf::RecordIdentity& identity) const {
  osc::ExactObject objects[osf::kMaxExactCustodyObjects]{};
  uint16_t count = 0U;
  if (!records_.snapshotCustodyCandidates(objects, count) || count == 0U)
    return false;
  // Newest cached first: it is the closest thing to live data we may send
  // without sealing (and caching) yet another object.
  // Incarnations are random, not ordered: prefer this boot's incarnation.
  const uint64_t current = records_.store().incarnation();
  osf::RecordIdentity best = objects[0].identity;
  for (uint16_t i = 1U; i < count && i < osf::kMaxExactCustodyObjects; ++i) {
    const osf::RecordIdentity& id = objects[i].identity;
    const bool id_now = id.incarnation == current;
    const bool best_now = best.incarnation == current;
    if ((id_now && !best_now) ||
        (id_now == best_now && id.incarnation == best.incarnation &&
         id.sequence > best.sequence))
      best = id;
  }
  if (!best.valid()) return false;
  identity = best;
  return true;
}

bool TrackerProductTxPilot::rearm() {
  if (phase_ != Phase::kDone) return false;
  identity_ = {};
  sealed_ = {};
  phase_ = Phase::kSelect;
  return true;
}

TrackerProductTxPilot::Event TrackerProductTxPilot::update(bool eligible) {
  if (phase_ == Phase::kDone || phase_ == Phase::kBlocked)
    return Event::kNone;

  // The store poll is externally driven. Never send on the strength of a
  // kStarted result; demand a durable exact-byte cache outcome/readback.
  if (phase_ == Phase::kWaitingCache) {
    ProductRecordStore::CacheOutcome outcome{};
    if (!records_.takeCacheOutcome(outcome)) return Event::kNone;
    if (!outcome.durable || !sameIdentity(outcome.identity)) {
      sealed_ = {};
      phase_ = Phase::kBlocked;
      return Event::kBlocked;
    }
    sealed_ = {};
    phase_ = Phase::kReady;
    return Event::kCacheDurable;
  }

  // Idle RF and store are prerequisites even for sealing: it reserves a
  // SecurityStore counter and may need a flash mutation.
  if (!eligible || !storageIdle() || !radio_.canSend())
    return Event::kNone;

  if (phase_ == Phase::kSelect && live_only_) {
    ObservationStore::Record newest{};
    if (!selectLive(newest)) {
      // Nothing new, or the newest cannot get a cache slot because four
      // unanswered objects hold them (e.g. out of range). Keep the link
      // alive with the newest cached object; an ACK then frees a slot.
      uint16_t occupied = 0U;
      osf::RecordIdentity fallback{};
      if (!records_.store().exactObjectCount(occupied) ||
          occupied < osf::kMaxExactCustodyObjects ||
          !selectCachedFallback(fallback))
        return Event::kNone;  // wait for the next new record
      identity_ = fallback;
      phase_ = Phase::kReady;
    }
  }

  if (phase_ == Phase::kSelect) {
    ObservationStore::Record record{};
    if (!((live_first_ || live_only_) && selectLive(record))) {
      const auto read = records_.oldestRetained(record);
      if (read == ObservationStore::LookupResult::kReadError) {
        phase_ = Phase::kBlocked;
        return Event::kBlocked;
      }
      if (read != ObservationStore::LookupResult::kFound)
        return Event::kNone;
    }
    if (!record.handle.identity.valid() || record.released ||
        record.release_uncertain || record.release_marker_exhausted ||
        record.kind != osf::RecordKind::kPeriodic ||
        record.schema != osf::kProductSchemaV1 ||
        record.payload_size != tlp::kPeriodicPlaintextSize) {
      phase_ = Phase::kBlocked;
      return Event::kBlocked;
    }

    identity_ = record.handle.identity;
    osc::ExactObject cached{};
    const auto found = records_.findCachedExact(identity_, cached);
    if (found == ObservationStore::ControlLookupResult::kReadError) {
      phase_ = Phase::kBlocked;
      return Event::kBlocked;
    }
    if (found == ObservationStore::ControlLookupResult::kFound) {
      // A cached exact object wins over fresh encryption, including reboot.
      phase_ = Phase::kReady;
    } else {
      uint16_t occupied = 0U;
      if (!records_.store().exactObjectCount(occupied) ||
          occupied >= osf::kMaxExactCustodyObjects) {
        // The record stays retained; never evict another custody object.
        // Live-first: keep moving by re-sending an already cached object.
        osf::RecordIdentity fallback{};
        if ((live_first_ || live_only_) && selectCachedFallback(fallback)) {
          identity_ = fallback;
          phase_ = Phase::kReady;
        } else {
          phase_ = Phase::kBlocked;
          return Event::kBlocked;
        }
      } else {
      tlp::PeriodicObservationPlaintext plaintext{};
      if (tlp::decodePeriodicPlaintext(
              record.payload, record.payload_size, &plaintext) !=
              tlp::ProductSecureCodecResult::kOk ||
          plaintext.record_incarnation != identity_.incarnation ||
          plaintext.record_sequence != identity_.sequence) {
        phase_ = Phase::kBlocked;
        return Event::kBlocked;
      }

      sealed_ = {};
      sealed_.identity = identity_;
      sealed_.record_kind = osf::RecordKind::kPeriodic;
      sealed_.object_size = static_cast<uint8_t>(tlp::kPeriodicFrameSize);
      // These immutable path bits are authenticated AAD. Permit eventual
      // one-hop relay without ever modifying an already sealed ciphertext.
      uint8_t wire[tlp::kPeriodicFrameSize]{};
      const auto result = crypto_.protectNextPeriodic(
          tlp::kProductPathFlagRelayAllowed |
              tlp::kProductPathFlagCustodyRequested,
          plaintext, wire);
      if (result == ProductSecureCryptoResult::kUnavailable) {
        sealed_ = {};
        return Event::kNone;
      }
      if (result != ProductSecureCryptoResult::kOk) {
        sealed_ = {};
        phase_ = Phase::kBlocked;
        return Event::kBlocked;
      }
      for (size_t i = 0U; i < sizeof(wire); ++i)
        sealed_.object[i] = wire[i];
      for (size_t i = 0U; i < sizeof(wire); ++i) wire[i] = 0U;
      phase_ = Phase::kNeedCache;
      }
    }
  }

  if (phase_ == Phase::kNeedCache) {
    const auto admission = records_.submitExactCache(sealed_);
    if (admission == ProductRecordStore::CacheSubmit::kBusy)
      return Event::kNone;  // Keep SAME sealed bytes in RAM.
    if (admission == ProductRecordStore::CacheSubmit::kStarted) {
      phase_ = Phase::kWaitingCache;
      return Event::kCachePending;
    }
    if (admission == ProductRecordStore::CacheSubmit::kAlreadyDurable) {
      sealed_ = {};
      phase_ = Phase::kReady;
    } else {
      sealed_ = {};
      phase_ = Phase::kBlocked;
      return Event::kBlocked;
    }
  }

  if (phase_ != Phase::kReady) return Event::kNone;

  osc::ExactObject exact{};
  if (records_.findCachedExact(identity_, exact) !=
          ObservationStore::ControlLookupResult::kFound ||
      !sameIdentity(exact.identity) ||
      !records_.custodyCandidateMatchesRetainedExact(exact)) {
    phase_ = Phase::kBlocked;
    return Event::kBlocked;
  }
  tlp::ProductSecureFrame header{};
  if (tlp::decodeProductSecureFrame(
          exact.object, exact.object_size, &header) !=
          tlp::ProductSecureCodecResult::kOk ||
      header.device_id != radio_.deviceId() ||
      header.app_family != tlp::kProductAppFamilyPeriodic ||
      (header.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U) {
    // An older exact object may have been sealed as direct-only. Send it
    // unchanged on the direct pilot path; never enable relay by rewriting AAD.
    phase_ = Phase::kBlocked;
    return Event::kBlocked;
  }

  if (!radio_.sendProductSecurePacket(exact.object, exact.object_size))
    return Event::kNone;
  phase_ = Phase::kDone;  // TX start, not Gateway receipt or release.
  return Event::kSent;
}

}  // namespace orun_tlp
