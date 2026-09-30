#pragma once

#include "device_identity.h"
#include "gnss_fix.h"
#include "history_store.h"
#include "radio_manager.h"

namespace orun_tlp {
class PositionFlow {
 public:
  enum class Event { kNone, kStored, kStorageFailure, kLiveExpired };
  PositionFlow(HistoryStore& store, RadioManager& radio)
      : store_(store),
        radio_(radio),
        device_identity_(radio.deviceIdentity()) {}
  void setDeviceIdentity(DeviceIdentity identity) { device_identity_ = identity; }
  // A fresh fix is not consumed while HistoryStore is lazily reserving the
  // first post-boot ticket block. Failed/unready storage keeps the existing
  // fail-stop behavior: the attempted fix is consumed and rejected explicitly.
  bool canAcceptFix() {
    return !appending_ &&
           (!store_.ready() || store_.prepareAppend());
  }
  bool acceptFix(const GnssFix& fix, uint32_t now);
  // M6D2: the geofence coordinator may choose a fix which was already accepted
  // as fresh Location evidence, then wait behind an existing History append.
  // Preserve that accepted observation in history even if it ages past the
  // live-freshness window while waiting. update() still applies the unchanged
  // live-age gate before any RF transmission, so stale data is never presented
  // as current.
  bool acceptPreviouslyAcceptedFix(const GnssFix& fix, uint32_t now);
  Event update(uint32_t now, bool allow_live_tx = true);
  bool pending() const { return appending_ || live_pending_; }
  uint32_t storageDrops() const { return storage_drops_; }
 private:
  bool acceptFixInternal(const GnssFix& fix, uint32_t now,
                         bool require_current_freshness);

  HistoryStore& store_;
  RadioManager& radio_;
  DeviceIdentity device_identity_{};
  uint8_t packet_[tlp::kPositionPacketSize]{};
  uint64_t record_identity_ = 0;
  uint32_t captured_at_ms_ = 0, storage_drops_ = 0;
  bool appending_ = false, live_pending_ = false;
};
}  // namespace orun_tlp
