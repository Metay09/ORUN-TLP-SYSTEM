#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kq05_gateway_durable_batch_owner.h"
#include "product_custody_store.h"

namespace orun_tlp {

// KQ05 host-only next step: bounded Gateway loop-task group collector.
// The ONLY contents of the group are 1..5 committed OCQ2 handles; there is
// no extra FIFO of historical ciphertexts or range-based custody authority.
// A physical Gateway composition root must serialize this object, grant
// mutations, radio TX and the single ProductCustodyStore Flash owner.
//
// This class is NOT part of the current RF/runtime path. Its caller must
// not treat "prepared" as an actually transmitted ACK.
class Kq05GatewayBatchCollector final {
 public:
  enum class Receive : uint8_t {
    kStartedFlash,    // Not durable or eligible for ACK yet
    kQueued,          // Duplicate or immediately readback-verified handle
    kDuplicate,       // Already in this exact group; never count twice
    kBusy,            // Group/Flash work outstanding, retry RX
    kDenied
  };
  enum class Service : uint8_t {
    kIdle, kWaitingFlash, kQueued, kRejected
  };
  enum class Prepare : uint8_t {
    kReady, kNotReady, kStorageBusy, kDenied
  };

  Kq05GatewayBatchCollector(ProductCustodyStore& store,
                            GatewayCustodyGrantProvider& grants,
                            uint64_t local_gateway_id)
      : store_(store), grants_(grants),
        signer_(store, grants, local_gateway_id),
        gateway_id_(local_gateway_id) {}
  Kq05GatewayBatchCollector(const Kq05GatewayBatchCollector&) = delete;
  Kq05GatewayBatchCollector& operator=(
      const Kq05GatewayBatchCollector&) = delete;

  // Must be called on the same Gateway loop task as the physical
  // ProductCustodyStore owner. Incoming exact object is untrusted.
  Receive receive(const uint8_t* object, size_t length);
  // Drives at most the one in-flight Flash admission. Does not touch RF.
  Service service();

  // Explicit, trusted loop scheduling decision for 1..4 accumulated
  // records, e.g. bounded latency deadline. The fifth auto-flushes.
  // A caller must NEVER pass radio-supplied "flush" timing as authority.
  bool requestFlush();
  // Re-reads each committed OCQ2 handle and rechecks CURRENT grant
  // before each prepared type 0x0A frame, preserving output on failure.
  // The queue is still retained for failed radio admission/retry.
  Prepare prepare(uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
                  size_t& length);
  // Call ONLY after a trusted RF owner has actually accepted the prepared
  // frame for TX; never when canSend() returns false. This acknowledges
  // LOCAL enqueue, NOT Tracker receipt. Retransmissions can re-enroll
  // duplicate durable objects and produce another authenticated ACK.
  bool markTxAdmitted();

  uint64_t localGatewayId() const { return gateway_id_; }
  uint8_t queued() const { return count_; }
  bool pendingFlash() const { return waiting_flash_; }
  bool flushRequested() const { return flush_requested_; }
  bool prepared() const { return prepared_; }

 private:
  bool addCommitted(const ProductCustodyStore::Handle& handle,
                    const uint8_t* expected, size_t length);
  void clearFlashPending();
  void resetGroup();

  ProductCustodyStore& store_;
  GatewayCustodyGrantProvider& grants_;
  Kq05GatewayDurableBatchOwner signer_;
  uint64_t gateway_id_ = 0U;
  ProductCustodyStore::Handle handles_[kq05_gateway_batch_ack::kMaxObjects]{};
  uint8_t count_ = 0U;
  uint64_t tracker_id_ = 0U;
  uint32_t key_epoch_ = UINT32_MAX;
  bool waiting_flash_ = false;
  bool flush_requested_ = false;
  bool prepared_ = false;
  size_t pending_size_ = 0U;
  uint8_t pending_object_[custody_store_product_format::kMaxObjectSize]{};
};

} // namespace orun_tlp
