#include "position_flow.h"
#include "gnss_manager.h"
#include "gnss_config.h"
#include "monotonic_time.h"

namespace orun_tlp {
bool PositionFlow::acceptFix(const GnssFix& fix, uint32_t now) {
  if (!canAcceptFix() || monotonic::elapsed(
          now, fix.captured_at_ms, gnss_config::kFreshFixMaxAgeMs)) return false;
  live_pending_ = false;  // New live data replaces an older unsent live candidate.
  if (!store_.ready() || !radio_.encodePosition(fix, packet_, identity_) ||
      !store_.append(packet_, identity_)) {
    ++storage_drops_; return false; // Strict store-first: never bypass persistence.
  }
  captured_at_ms_ = fix.captured_at_ms; appending_ = true; return true;
}
PositionFlow::Event PositionFlow::update(uint32_t now, bool allow_live_tx) {
  Event event = Event::kNone;
  bool stored;
  if (appending_ && store_.takeAppendResult(stored)) {
    appending_ = false;
    if (!stored) { ++storage_drops_; return Event::kStorageFailure; }
    live_pending_ = true; event = Event::kStored;
  }
  if (!allow_live_tx) {
    live_pending_ = false;
    return event;
  }
  if (live_pending_) {
    if (monotonic::elapsed(now, captured_at_ms_, gnss_config::kFreshFixMaxAgeMs)) {
      live_pending_ = false; return Event::kLiveExpired; // Record remains backlog.
    }
    if (radio_.canSend()) {
      // Driver gate contention is a defer, not an attempted transmission.
      if (radio_.sendPositionPacket(packet_, &captured_at_ms_))
        live_pending_ = false; // TX_DONE is not delivery.
    }
  }
  return event;
}
}  // namespace orun_tlp
