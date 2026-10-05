#pragma once

#include <stddef.h>
#include <stdint.h>

#include "history_store.h"
#include "tlp_v2_history_secure.h"

namespace orun_tlp {

// SF3A transport-neutral RAM owner for authenticated History delivery facts.
//
// Security ordering is intentionally outside this class for this first slice.
// applyReplayAcceptedBackendDurableReceipt() may be called ONLY after:
//   1. BACKEND_A2D History receipt AEAD authentication/decryption succeeded;
//   2. SecurityStore accepted the exact authenticated
//      (credential_id, key_epoch, security_counter) replay tuple.
//
// The caller must also pass the exact authenticated History incarnation from
// the same successfully opened packet. The coordinator never re-reads or
// invents remote security context.
//
// This class then owns only application-delivery RAM state. It never mutates
// SecurityStore, never sends RF, never persists replay cursors and never writes
// a History delivery checkpoint.
class HistoryDeliveryCoordinator {
 public:
  // Two maximum-size SF2 receipt batches. This keeps the initial selective
  // acknowledgement window explicitly bounded to 96 bytes of identities.
  static constexpr size_t kMaxSelectiveAcknowledgements =
      2U * tlp::kHistoryReceiptMaxIdentities;

  enum class ApplyResult : uint8_t {
    kApplied,
    kDuplicateOnly,
    kUnavailable,
    kInvalidReceipt,
    kUnknownIdentity,
    kSelectiveSetFull,
    kInvariantFailure,
  };

  struct Diagnostics {
    uint32_t applied_receipts = 0;
    uint32_t duplicate_receipts = 0;
    uint32_t unknown_identity_rejections = 0;
    uint32_t capacity_rejections = 0;
    uint32_t contiguous_advances = 0;
    uint32_t stale_selective_prunes = 0;
    uint32_t invariant_failures = 0;
  };

  explicit HistoryDeliveryCoordinator(HistoryStore& history)
      : history_(history) {}

  // Applies one BACKEND_DURABLE receipt whose transport replay admission has
  // already been accepted by SecurityStore.
  //
  // The operation is preflighted before mutating History RAM state:
  // - every new identity must still be an actual retained History record;
  // - numeric ticket gaps are never treated as observations;
  // - newer explicit identities may wait in the bounded RAM selective set;
  // - only a contiguous prefix of actual records advances acknowledgedThrough.
  //
  // All ordinary rejection results are atomic with respect to HistoryStore's
  // RAM delivery watermark. kInvariantFailure is different: a transient read
  // fault may occur after one or more individually strict-validated prefix
  // records were safely acknowledged. That safe RAM prefix is never rolled
  // back; callers must re-read acknowledgedThrough() after kInvariantFailure.
  //
  // No durable checkpoint is written by this method. SF3 checkpoint policy is
  // a separate wear-sensitive decision.
  ApplyResult applyReplayAcceptedBackendDurableReceipt(
      const tlp::BackendDurableReceiptPlaintext& receipt,
      uint64_t authenticated_history_incarnation);

  size_t selectiveAcknowledgementCount() const {
    return selective_count_;
  }
  uint64_t acknowledgedThrough() const {
    return history_.acknowledgedThrough();
  }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  static bool findIdentity(const uint64_t* ids, size_t count, uint64_t id,
                           size_t* index = nullptr);
  static void eraseIdentityAt(uint64_t* ids, size_t& count, size_t index);
  static bool insertIdentitySorted(uint64_t* ids, size_t& count,
                                   size_t capacity, uint64_t id);

  HistoryStore& history_;
  uint64_t selective_ids_[kMaxSelectiveAcknowledgements]{};
  size_t selective_count_ = 0;
  uint64_t bound_history_incarnation_ = 0;
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
