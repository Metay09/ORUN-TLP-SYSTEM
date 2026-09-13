#include "position_flow.h"
#include "gnss_config.h"
#include "monotonic_time.h"

namespace orun_tlp {
bool PositionFlow::acceptFix(const GnssFix& fix, uint32_t now) {
  if (!canAcceptFix()) return false;
  live_pending_ = false;  // New live data replaces an older unsent live candidate.
  if (!store_.ready() || !radio_.encodePosition(fix, packet_, identity_) ||
      !store_.append(packet_, identity_)) {
    ++storage_drops_; return false; // Strict store-first: never bypass persistence.
  }
  accepted_at_ = now; appending_ = true; return true;
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
    if (monotonic::elapsed(now, accepted_at_, gnss_config::kFreshFixMaxAgeMs)) {
      live_pending_ = false; return Event::kLiveExpired; // Record remains backlog.
    }
    if (radio_.canSend()) {
      (void)radio_.sendPositionPacket(packet_);
      live_pending_ = false; // One live attempt. TX_DONE is not delivery.
    }
  }
  return event;
}
}  // namespace orun_tlp
