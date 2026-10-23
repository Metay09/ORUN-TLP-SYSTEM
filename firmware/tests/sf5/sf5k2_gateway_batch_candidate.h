#pragma once

// SF5K2 HOST-ONLY / DORMANT candidate. Produces no wire ACK, MAC, trusted
// grant, flash release, or radio operation. Provider is a HOST TEST SEAM;
// only a future reviewed adapter can supply authenticated Edge proof.
#include <stddef.h>
#include <stdint.h>
#include "sf5g_custody_record_candidate.h"

namespace orun_tlp {
namespace sf5k2_candidate {
namespace rec = sf5g_custody_candidate;
constexpr size_t kWindow = 4U;

struct Scope {
  uint64_t tracker_id = 0U;
  uint64_t gateway_id = 0U;
  uint32_t key_epoch = 0U;
  uint32_t policy_floor = 0U;
  uint32_t grant_generation = 0U;
};

struct Pending {
  const uint8_t* exact = nullptr; // valid ONLY during build()
  size_t size = 0U;
  bool local_mutation_started = false; // uncertain local flash => no Edge shortcut
};

enum class EdgeFact : uint8_t {
  kUnavailable,
  kTransportOnly,
  kAuthenticatedDurableExact, // MOCK result; no production proof today
};

class EvidencePort {
 public:
  virtual ~EvidencePort() {}
  // Returns true only when the complete 120-byte *readback* is available.
  // Pending/timeout/error/uncertain flash cannot return synthetic success.
  virtual bool readLocalCandidate(size_t request_index,
                                  uint8_t out[rec::kRecordSize]) = 0;
  // Must eventually be driven by the reviewed SF5G Edge authenticator and
  // durable anti-replay owner, NOT transport ACK, BLE, HTTP or a boolean.
  virtual EdgeFact edgeFact(size_t request_index, const Scope& scope,
                            const uint8_t* exact, size_t size) = 0;
};

struct ExactMember {
  uint8_t size = 0U;
  uint8_t bytes[rec::kObjectMax]{};
  uint8_t input_index = 0U;
};

struct PreparedCandidate {
  Scope scope{};
  uint8_t count = 0U;
  ExactMember members[kWindow]{};
};

enum class Result : uint8_t {
  kInvalid,       // all-or-nothing malformed request: output unchanged
  kNoDurableFact, // valid request; no eligible items: output unchanged
  kPrepared,      // untrusted staging only: NOT custody or ACK authority
};

// All incoming objects are *structurally* checked, but not cryptographically
// authenticated; the future production Gateway verifier is a separate gate.
// No packet refs, SHA256, HMAC, 0x06 wire frame, signing or record deletion.
// Duplicate exact inputs/mixed tracker/epoch fail entire request; uncertain
// members are omitted individually, never accepted by a cumulative watermark.
Result prepare(const Scope& scope, const Pending* objects, size_t count,
               EvidencePort& evidence, PreparedCandidate* out);

} // namespace sf5k2_candidate
} // namespace orun_tlp
