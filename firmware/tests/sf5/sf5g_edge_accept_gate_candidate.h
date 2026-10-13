#pragma once

// SF5G HOST-ONLY edge durable-accept CONTRACT GATE.
// NO actual receipt wire format, cryptographic verification, flash erase,
// authenticated custody ACK, or production Gateway runtime is implemented.
#include <stddef.h>
#include <stdint.h>

#include "sf5g_custody_record_candidate.h"
#include "sf5g_page_header_candidate.h"

namespace orun_tlp {
namespace sf5g_edge_accept_candidate {

namespace rec = sf5g_custody_candidate;
namespace page = sf5g_page_candidate;

// The pending exact sealed product object and the specific send attempt.
// These are host MODEL identities, NOT approved receipt wire fields.
struct Attempt {
  uint64_t gateway_instance = 0U;
  uint64_t attempt_id = 0U;
  uint64_t edge_issuer = 0U;
  uint16_t source_page = UINT16_MAX;
  uint16_t source_slot = UINT16_MAX;
  uint64_t source_generation = 0U;
  uint8_t object[rec::kObjectMax]{};
  size_t object_size = 0U;
  bool local_fallback_started = false;
};

enum class DurableScope : uint8_t {
  kTransportOnly,
  kEdgeDurableCommitted,
};

// Only a FUTURE independently reviewed authenticator may populate this.
// It must cryptographically authenticate issuer, epoch/counter, exact bytes,
// durable-accept semantics and all source/attempt binding fields together.
// A plain Edge HTTP 200/socket ACK is not an authenticated receipt.
// Public struct is intentional ONLY for host mock injection; do not use
// it as a production proof/capability.
struct VerifiedClaim {
  uint64_t gateway_instance = 0U;
  uint64_t attempt_id = 0U;
  uint64_t edge_issuer = 0U;
  uint16_t source_page = UINT16_MAX;
  uint16_t source_slot = UINT16_MAX;
  uint64_t source_generation = 0U;
  uint8_t object[rec::kObjectMax]{};
  size_t object_size = 0U;
  uint32_t key_epoch = 0U;
  uint64_t security_counter = 0U;
  DurableScope scope = DurableScope::kTransportOnly;
};

enum class VerifyStatus : uint8_t { kVerified, kRejected, kUnavailable };

class ReceiptVerifier {
 public:
  virtual ~ReceiptVerifier() {}
  // The raw receipt bytes have no fixed format in this stage.
  virtual VerifyStatus verify(const uint8_t* raw, size_t raw_size,
                              VerifiedClaim& output) const = 0;
};

enum class ReplayStatus : uint8_t {
  kDurablyAccepted, // durable write/replay reservation + readback completed
  kReplay,          // duplicate/old counter, or epoch/issuer mismatch
  kUnavailable,     // flash unavailable/busy/uncertain, no proof of commit
};

class ReplayStore {
 public:
  virtual ~ReplayStore() {}
  // Must bind security state to authenticated Edge issuer + credential epoch,
  // reject replay and be retained across power loss. No volatile-only success.
  virtual ReplayStatus admitDurably(uint64_t issuer, uint32_t key_epoch,
                                    uint64_t counter) = 0;
};

enum class GateResult : uint8_t {
  kCandidateOnly,        // qualified model input; NOT an ACK/erase grant
  kInvalidAttempt,
  kFallbackInProgress,
  kAuthRejected,
  kAuthUnavailable,
  kBindingMismatch,
  kReplayRejected,
  kReplayUnavailable,
};

// One-shot synchronous seam. Any failure => no custody release.
// On successful mock replay reservation it yields only a host-only
// candidate classification; NO side effects on custody/flash/ACK.
GateResult evaluate(const Attempt& pending, uint16_t actual_page_count,
                    const uint8_t* raw_receipt, size_t receipt_size,
                    const ReceiptVerifier& verifier, ReplayStore& replay);

}  // namespace sf5g_edge_accept_candidate
}  // namespace orun_tlp
