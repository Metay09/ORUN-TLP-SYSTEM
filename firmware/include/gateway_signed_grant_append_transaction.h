#pragma once

#include <stdint.h>

#include "gateway_signed_grant_append_preflight.h"

// SF5K3AC TEST-ONLY: This is a simulator of a future Gateway-only flash
// writer. There is NO live provisioning surface and the normal firmware
// cannot instantiate this class. Never enable on field hardware.
#if defined(ORUN_SF5K3AC_TEST_ONLY_TRANSACTION)
namespace orun_tlp {

class GatewaySignedGrantAppendTransaction final {
 public:
  enum class State : uint8_t {
    kIdle, kBusy, kCommittedAwaitingAnchor, kFault
  };
  explicit GatewaySignedGrantAppendTransaction(FlashBackend& exclusive_flash)
      : flash_(exclusive_flash) {}
  ~GatewaySignedGrantAppendTransaction();
  GatewaySignedGrantAppendTransaction(
      const GatewaySignedGrantAppendTransaction&) = delete;
  GatewaySignedGrantAppendTransaction& operator=(
      const GatewaySignedGrantAppendTransaction&) = delete;

  // Requires exclusive Gateway security partition ownership AND fully
  // quiesced RF/ACK clients. Candidate and current share the SAME pinned
  // verifier instance. An independently trusted monotonic anchor is
  // obligatory; the test fake is not such an implementation.
  //
  // Re-validates all signed input and current persisted bytes BEFORE
  // any mutation, then invalidates the old in-RAM grant authority. It
  // erases ONLY the inactive security page; never the current one.
  bool start(GatewayGrantSnapshotReader& current,
             const GatewayGrantSnapshotReader& candidate,
             const GatewayGrantRevisionAnchor& anchor,
             uint64_t trusted_gateway_id, bool security_owner_exclusive,
             bool rf_and_ack_quiesced);

  // One flash mutation or verification step; async completions poll once
  // per call and are NEVER resubmitted. Readback is required at each step.
  // On every error latch Fault; never auto-retry unknown outcomes.
  State poll();
  State state() const { return state_; }
  uint16_t destinationPage() const { return destination_; }

 private:
  enum class Phase : uint8_t {
    kErase, kBody, kPreCommit, kCommit
  };
  void fault();
  void wipe();
  bool exactRead(uint32_t offset, const uint8_t* expected,
                 size_t length) const;
  bool erasedPage(uint16_t page) const;
  bool verifyPreCommit() const;

  FlashBackend& flash_;
  const GatewayGrantRevisionAnchor* anchor_ = nullptr;
  const GatewayEnrollmentProofVerifier* issuer_ = nullptr;
  State state_ = State::kIdle;
  Phase phase_ = Phase::kErase;
  uint16_t source_ = UINT16_MAX;
  uint16_t destination_ = UINT16_MAX;
  uint64_t gateway_ = 0U;
  uint64_t current_revision_ = 0U;
  uint32_t offset_ = 0U;
  uint32_t current_length_ = 0U;
  bool waiting_ = false;
  uint8_t source_snapshot_[gateway_grant_format::kStoredBytes]{};
  uint8_t candidate_[gateway_grant_format::kStoredBytes]{};
};

}  // namespace orun_tlp
#endif
