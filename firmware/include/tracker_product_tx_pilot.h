#pragma once

#include <stdint.h>
#include "product_record_store.h"
#include "product_secure_crypto.h"
#include "radio_manager.h"

namespace orun_tlp {

// SF5K3V: exact-cache-before-RF pilot. The outer loop owns LoRa duty/
// ACK pacing; only read-side failure re-selection uses its own cooldown.
// The ProductRecordStore alone owns flash; crypto alone owns the TX nonce;
// RadioManager alone owns SX1262. No ACK/release authority exists here.
class TrackerProductTxPilot {
 public:
  enum class Event : uint8_t {
    kNone, kCachePending, kCacheDurable, kSent, kBlocked
  };

  // Read-only diagnosis for the permanently blocked bench pilot. Never
  // changes its custody, RF, encryption, or flash-write decisions.
  enum class BlockReason : uint8_t {
    kNone, kCacheCommitFailed, kCacheIdentityMismatch,
    kRetainedReadError, kRetainedInvalid, kExactLookupReadError,
    kExactCountUnavailable, kExactSlotsFull, kPlaintextInvalid,
    kCryptoFailed, kCacheSubmitRejected, kExactProofMismatch,
    kCachedHeaderInvalid
  };

  TrackerProductTxPilot(ProductRecordStore& records,
                        ProductSecureCrypto& crypto, RadioManager& radio)
      : records_(records), crypto_(crypto), radio_(radio) {}

  // Invoke after ProductRecordStore::poll() and higher-priority radio traffic.
  // Passing false still drains a completed cache outcome, but does not
  // initiate crypto/flash/RF. A sent object is never retried this boot.
  Event update(bool eligible);

  // SF5L2: after a TX, allow ONE more selection of the oldest retained
  // record. If the previous record was durably released this is the next
  // one; if not, it is the same record and its already cached exact bytes
  // are re-sent unchanged (never re-sealed). The caller owns pacing and
  // duty-cycle policy. Fatal blocked stays blocked; recoverable errors are
  // handled by recoverIfSafe() instead. true = re-armed.
  bool rearm();
  bool sent() const { return phase_ == Phase::kDone; }
  bool blocked() const { return phase_ == Phase::kBlocked; }
  BlockReason blockReason() const { return block_reason_; }
  const char* blockReasonName() const;
  // Retry only read-side/transient selection failures. Integrity, crypto,
  // unknown flash commit and malformed durable bytes stay fail-closed.
  bool retryableBlocked() const;
  // Called by the owning loop, never by a BLE/radio callback. Non-blocking,
  // wrap-safe timer; performs no flash mutation, nonce burn, or RF send.
  // A true result merely re-opens selection; update() revalidates everything.
  bool recoverIfSafe(uint32_t now_ms, bool eligible);
  uint32_t automaticRecoveries() const { return automatic_recoveries_; }
  uint32_t retryAttempts() const { return retry_attempts_; }
  // 0 if no record has been selected (e.g. a read error).
  uint32_t selectedSequence() const { return identity_.sequence; }

  // SF5L4 (bench): "live data must not wait behind the backlog". When on,
  // each selection first tries the NEWEST retained record, then the oldest.
  // If the 4-object exact cache is full, the newest already cached exact
  // object is re-sent unchanged instead of blocking. TX order only: ring
  // retention and release ownership are unchanged. Off by default (SF5K3V).
  void setLiveFirst(bool on) { live_first_ = on; idle_ = false; }
  bool liveFirst() const { return live_first_; }
  // SF5L4 range walks: send ONLY the newest record, right after it is
  // stored, so its radio facts belong to the place it was recorded. The
  // backlog stays retained (nothing is released or skipped) and drains
  // when this is switched off. Implies live-first.
  void setLiveOnly(bool on) { live_only_ = on; idle_ = false; }
  bool liveOnly() const { return live_only_; }
  // Identity of the record most recently selected (valid after a send).
  const osf::RecordIdentity& lastIdentity() const { return identity_; }

 private:
  enum class Phase : uint8_t {
    kSelect, kNeedCache, kWaitingCache, kReady, kDone, kBlocked
  };

  bool storageIdle() const;
  bool selectLive(ObservationStore::Record& record) const;
  bool selectCachedFallback(osf::RecordIdentity& identity) const;
  bool sameIdentity(const osf::RecordIdentity& other) const;

  ProductRecordStore& records_;
  ProductSecureCrypto& crypto_;
  RadioManager& radio_;
  Phase phase_ = Phase::kSelect;
  BlockReason block_reason_ = BlockReason::kNone;
  uint32_t retry_after_ms_ = 0U;
  uint32_t retry_attempts_ = 0U;
  uint32_t automatic_recoveries_ = 0U;
  bool retry_scheduled_ = false;
  osf::RecordIdentity identity_{};
  osc::ExactObject sealed_{};
  bool live_first_ = false;
  bool live_only_ = false;
  // "Nothing to send" was decided for this next-record identity. Until a new
  // record is appended (or the mode changes / rearm) nothing can change that
  // answer, so the selection does not re-scan the store on every loop pass.
  bool idle_ = false;
  osf::RecordIdentity idle_next_{};
  void markIdle();
  Event block(BlockReason reason);
};

}  // namespace orun_tlp
