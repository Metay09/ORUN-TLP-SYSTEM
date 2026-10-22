#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "gateway_product_startup_guard.h"

namespace orun_tlp {
namespace gateway_grant_format {

// SF5K3Y candidate: full-page, signed enrollment snapshot on Gateway-only
// security ownership. The 2x4 KiB physical SecurityStore partition cannot
// be mounted by Tracker SecurityStore and this store at the same time.
constexpr uint32_t kPageBytes = 4096U;
constexpr uint32_t kMagic = 0x4F474B31U;  // "OGK1", never "ORS1"
constexpr uint8_t kVersion = 1U;
constexpr uint16_t kPageCount = 2U;
constexpr size_t kHeaderBytes = 32U;
constexpr size_t kGrantBytes = 56U;
constexpr size_t kGrantSlots = 32U;
constexpr size_t kPayloadBytes = kHeaderBytes + kGrantBytes * kGrantSlots;
constexpr size_t kProofBytes = 64U;  // Detached authenticity proof, REQUIRED.
constexpr size_t kCrcOffset = kPayloadBytes + kProofBytes;
constexpr size_t kCommitOffset = kCrcOffset + 4U;
constexpr size_t kStoredBytes = kCommitOffset + 4U;
static_assert(kStoredBytes < kPageBytes, "Gateway grant page does not fit");

}  // namespace gateway_grant_format

// A separate, trusted provisioning authority must authenticate the *entire*
// canonical payload. Not an unkeyed CRC, received radio message, ORS1 root or
// a baked-in fake key. The future production verifier needs a reviewed, pinned
// issuer public key and cryptographic implementation. No production verifier
// exists in SF5K3Y.
class GatewayEnrollmentProofVerifier {
 public:
  virtual ~GatewayEnrollmentProofVerifier() = default;
  virtual bool verify(const uint8_t* canonical_payload, size_t payload_size,
                      const uint8_t (&proof)[gateway_grant_format::kProofBytes])
                      const = 0;
};

// Recovery-only, fail-closed source. Never provisions, writes, erases,
// generates or exports root secrets. Refuses ORS1/unknown/torn/foreign pages.
// activeGrant() exposes ONLY the exact active scoped K_custody_ack for a
// matched Tracker and epoch, never a Tracker K_root.
class GatewayGrantSnapshotReader final : public GatewayDurableGrantSource {
  // Read access to VERIFIED immutable snapshots only, for a pure proposal
  // preflight. No grant keys or writer capability escape this friendship.
  friend class GatewaySignedGrantAppendPreflight;
 public:
  enum class Status : uint8_t {
    kUnopened, kWrongOwner, kUnavailable, kEmpty, kRejected, kReady
  };

  GatewayGrantSnapshotReader(FlashBackend& flash,
                             const GatewayEnrollmentProofVerifier& verifier)
      : flash_(flash), verifier_(verifier) {}
  ~GatewayGrantSnapshotReader();

  // Security region must be exclusively Gateway-owned; the Tracker
  // SecurityStore may not be mounted. Reboot is required after a failure.
  Status begin(uint64_t local_gateway_id, bool gateway_security_owner,
               bool tracker_security_store_mounted);
  // Sticky emergency stop until next boot. NOT a durable revocation writer.
  void denyForCurrentBoot();

  bool verifiedForLocalGateway(uint64_t gateway_id) const override;
  bool activeGrant(uint64_t tracker_id, uint32_t key_epoch,
                   GatewayCustodyAckScope& scope,
                   uint8_t (&scoped_key)[32]) override;
  Status status() const { return status_; }
  // Read-only snapshot identity used ONLY with an independent trusted
  // rollback checkpoint. A recovered signature alone is not authority.
  uint64_t activeGeneration() const {
    return verifiedForLocalGateway(gateway_id_) ? active_generation_ : 0U;
  }
  uint64_t localGatewayId() const { return gateway_id_; }

 private:
  void wipeScratch();
  bool erasedPage(uint16_t page) const;
  bool scanPage(uint16_t page, bool& present, uint64_t& generation);
  bool validateSlots() const;

  FlashBackend& flash_;
  const GatewayEnrollmentProofVerifier& verifier_;
  Status status_ = Status::kUnopened;
  uint64_t gateway_id_ = 0U;
  uint16_t active_page_ = UINT16_MAX;
  uint64_t active_generation_ = 0U;
  bool denied_ = false;
  // Sized to one canonical payload/proof/CRC/commit, not to a whole page.
  uint8_t scratch_[gateway_grant_format::kStoredBytes]{};
};

}  // namespace orun_tlp
