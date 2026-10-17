#pragma once

// SF5K3C candidate only: version-3 security journal byte contract.
// NOT wired into SecurityStore, the radio, credential provisioning or ACK
// release. The shipping SecurityStore continues to write v2.
//
// The existing security partition remains TWO 4096-byte A/B pages.
// Each v3 page has the unchanged 32-byte header + 68-byte credential and
// 71 uniformly sized 56-byte state entries, leaving 20 erased tail bytes.
#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace sf5k3c_candidate {

constexpr uint8_t kCandidateVersion = 3U;
constexpr size_t kPageSize = 4096U;
constexpr size_t kHeaderSize = 32U;
constexpr size_t kCredentialSize = 68U;
constexpr size_t kEntrySize = 56U;
constexpr size_t kSlotCount = (kPageSize - kHeaderSize - kCredentialSize) /
                              kEntrySize;
constexpr size_t kTailBytes = kPageSize - kHeaderSize - kCredentialSize -
                              kSlotCount * kEntrySize;
constexpr size_t kMaxGateways = 4U;
static_assert(kSlotCount == 71U && kTailBytes == 20U,
              "v3 candidate must not change physical security partition");

enum class Kind : uint8_t {
  kTxReserve = 1U,
  kA2dReplayReserve = 2U,
  kCustodyPolicy = 3U,
  // SF5K3D host-only, valid ONLY as a complete 5-entry page prefix.
  kPolicySnapshot = 4U,
  kGatewaySnapshot = 5U,
};

// Logical 56-byte state journal entry. CRC and COMMIT are not struct fields.
// Counter kinds store their exclusive bound in value with slot=0xFF.
// Policy kind uses slot 0..3 (or 0xFF for floor-only), value=gateway ID
// (zero for revoke), grant_generation, policy floor and policy_revision.
// Each grant/revoke monotonically advances its own slot generation and the
// revision. The global floor advances ONLY on an explicit policy-floor update,
// so unrelated Gateway keys are not invalidated by a single-slot change.
// Revoked slots preserve their last generation high-water as a tombstone.
struct Entry {
  uint8_t credential_id[16]{};
  uint32_t key_epoch = 0U;
  Kind kind = Kind::kTxReserve;
  uint8_t slot = 0xFFU;
  uint64_t value = 0U;
  uint32_t grant_generation = 0U;
  uint32_t custody_floor = 0U;
  uint32_t policy_revision = 0U;
};

struct GatewaySlot {
  uint64_t gateway_id = 0U;
  uint32_t generation = 0U;
};
struct State {
  uint8_t credential_id[16]{};
  uint32_t key_epoch = 0U;
  uint64_t tx_bound = 0U;
  uint64_t a2d_bound = 0U;
  uint32_t custody_floor = 0U;   // 0 is a valid initial floor
  uint32_t policy_revision = 0U;
  GatewaySlot gateways[kMaxGateways]{};
};

// Full emitted bytes include CRC and committed marker. The writer MUST
// program the first 52 bytes, verify readback, then program COMMIT word
// last under the exclusive SecurityStore flash owner. This codec is not
// a writer and must not be used as authenticated policy admission.
bool encodeEntry(const Entry& value, uint8_t (&bytes)[kEntrySize]);
bool decodeEntry(const uint8_t (&bytes)[kEntrySize], Entry& value);

// v3 header uses the same existing ORS1 page-header physical geometry,
// with version=3. No v3 header may be activated until the matching credential,
// TX/A2D snapshots and Gateway policy have been fully verified.
bool encodePageHeader(uint64_t generation, uint64_t tracker,
                      uint8_t (&bytes)[kHeaderSize]);
bool decodePageHeader(const uint8_t (&bytes)[kHeaderSize],
                      uint64_t tracker, uint64_t& generation);

// Host-model only: caller's `authenticated` truth is a test precondition,
// NEVER derived from an untrusted ACK. A real authorization owner must
// establish it through existing independently verified provisioning.
bool applyHostAuthenticated(const Entry& record, bool authenticated,
                            State& state);

// Scan the fixed v3 journal region (all 71 entries + erased tail). A
// staged non-committed entry burns its slot; corrupt committed data,
// non-erased tail, holes or stale monotonic values fail closed. Does not
// return a partially replayed state on any error.
bool recoverHostJournal(const uint8_t* region, size_t region_size,
                        const State& initial, State& out);

// Read-only exact durable policy-scope check, NEVER ACK HMAC verification.
bool grantScopeMatches(const State& state, uint64_t gateway_id,
                       uint32_t floor, uint32_t generation);

}  // namespace sf5k3c_candidate
}  // namespace orun_tlp
