#pragma once
#include "history_store.h"
#include "radio_manager.h"

namespace orun_tlp {
class PositionFlow {
 public:
  enum class Event { kNone, kStored, kStorageFailure, kLiveExpired };
  PositionFlow(HistoryStore& store, RadioManager& radio) : store_(store), radio_(radio) {}
  bool canAcceptFix() const { return !appending_ && (!store_.ready() || store_.canAppend()); }
  bool acceptFix(const GnssFix& fix, uint32_t now);
  Event update(uint32_t now, bool allow_live_tx = true);
  bool pending() const { return appending_ || live_pending_; }
  uint32_t storageDrops() const { return storage_drops_; }
 private:
  HistoryStore& store_;
  RadioManager& radio_;
  uint8_t packet_[tlp::kPositionPacketSize]{};
  uint64_t identity_ = 0;
  uint32_t captured_at_ms_ = 0, storage_drops_ = 0;
  bool appending_ = false, live_pending_ = false;
};
}  // namespace orun_tlp
