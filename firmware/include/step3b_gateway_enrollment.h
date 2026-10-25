#pragma once

// Step 3B (bench Gateway only): install a backend-signed OGK1 grant
// snapshot that arrives over USB into the Gateway-owned security region.
//
// Authority: USB physical access (ADR M7P6 §11) delivers the bytes; the
// pinned issuer signature, the exact local Gateway id and a strictly higher
// snapshot generation decide whether they are installed. Nothing received is
// trusted before GatewayGrantSnapshotReader has verified it (same reader and
// same pinned verifier as the boot path).
//
// Writer discipline (mirrors the SF5K3AC simulator): the boot snapshot is
// denied for the rest of the boot BEFORE any mutation; only the inactive
// page is erased; the body is programmed and read back, the commit word is
// programmed last, then the whole page is read back; an asynchronous
// completion is polled and never resubmitted; every flash error latches
// kFault (reboot required). The new snapshot is used after a reboot.
//
// Deviation from SF5K3Z (documented in docs/milestones/STEP3B.md): the
// rollback anchor is the generation of the snapshot already on the Gateway,
// not an independent monotonic checkpoint, and a blank region accepts the
// first snapshot. Someone with USB access can still CLEAN the region.
#if defined(ORUN_STEP3B_GATEWAY_ENROLLMENT)

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "gateway_grant_snapshot_reader.h"

namespace orun_tlp {

// Read-only view of one staged OGK1 image as a 2-page security region:
// page 0 = the staged bytes then 0xFF; page 1 erased. Lets the unmodified
// boot reader verify a candidate before anything touches real flash.
class Step3bStagedSnapshotFlash final : public FlashBackend {
 public:
  explicit Step3bStagedSnapshotFlash(const uint8_t* image) : image_(image) {}
  bool begin() override { return image_ != nullptr; }
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t, const void*, size_t) override {
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }

 private:
  const uint8_t* image_;
};

class Step3bGrantInstaller final {
 public:
  enum class State : uint8_t { kIdle, kReceiving, kWriting, kDone, kFault };
  enum class Reply : uint8_t {
    kNotMine,          // not a "BENCH OGK1 " line
    kOk,               // BEGIN / DATA accepted
    kAborted,
    kWriteStarted,     // COMMIT verified; writing the inactive page
    kMalformed,
    kBusy,             // writing / done / fault: reboot first
    kNotReceiving,
    kBadOffset,
    kIncomplete,
    kBadCrc,
    kWrongGateway,
    kNotAuthentic,     // format, slots or issuer signature
    kNotNewer,         // generation <= the installed one
    kRegionNotReady,   // installed region unreadable/foreign: CLEAN first
  };

  Step3bGrantInstaller(FlashBackend& security_region,
                       const GatewayEnrollmentProofVerifier& issuer,
                       uint64_t local_gateway_id)
      : flash_(security_region), issuer_(issuer), gateway_id_(local_gateway_id) {}
  ~Step3bGrantInstaller();
  Step3bGrantInstaller(const Step3bGrantInstaller&) = delete;
  Step3bGrantInstaller& operator=(const Step3bGrantInstaller&) = delete;

  // "BENCH OGK1 BEGIN" | "BENCH OGK1 DATA <offset 4 hex> <2..128 hex>" |
  // "BENCH OGK1 COMMIT <crc32 8 hex>" | "BENCH OGK1 ABORT".
  // `current` is the boot reader over the same security region.
  Reply handleLine(const char* line, size_t length,
                   GatewayGrantSnapshotReader& current);

  // One flash step (or one completion poll) per call.
  State poll();
  State state() const { return state_; }
  uint32_t received() const { return received_; }
  uint64_t candidateGeneration() const { return candidate_generation_; }
  uint16_t destinationPage() const { return destination_; }

  static const char* replyName(Reply reply);
  static const char* stateName(State state);

 private:
  enum class Phase : uint8_t { kErase, kBody, kCommit };
  Reply commit(uint32_t crc, GatewayGrantSnapshotReader& current);
  void wipe();
  void fault();
  bool exactRead(uint32_t offset, const uint8_t* expected, size_t length) const;
  bool erasedFrom(uint16_t page, uint32_t from) const;

  FlashBackend& flash_;
  const GatewayEnrollmentProofVerifier& issuer_;
  uint64_t gateway_id_;
  State state_ = State::kIdle;
  Phase phase_ = Phase::kErase;
  bool waiting_ = false;
  uint32_t received_ = 0U;
  uint32_t offset_ = 0U;
  uint32_t length_ = 0U;
  uint16_t destination_ = UINT16_MAX;
  uint64_t candidate_generation_ = 0U;
  uint8_t image_[gateway_grant_format::kStoredBytes]{};
};

}  // namespace orun_tlp
#endif
