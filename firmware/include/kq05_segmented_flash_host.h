#pragma once
// Compile-time HOST-ONLY adapter. Never include in a physical firmware image.
// There is NO production HistoryStore cutover, role provisioning, or
// SoftDevice arbitration proof in this file.
#ifndef ORUN_KQ05_SEGMENTED_HOST_TEST
#error "KQ05 segmented Flash adapter is HOST TEST ONLY; never link to RAK firmware"
#endif

#include <stddef.h>
#include <stdint.h>
#include "flash_backend.h"
#include "kq05_supplemental_28k_census.h"
#include "storage_config.h"

namespace orun_tlp {
namespace kq05_segmented_host {

// Logical page 0..31 => the existing Product 128 KiB physical partition;
// logical page 32..38 => the old History 28 KiB physical partition.
// The intervening 32 KiB of other physical owners is NEVER mapped.
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kPrimaryPages = storage_config::kProductRegionPages;
constexpr uint32_t kSupplementalPages = storage_config::kPageCount;
constexpr uint32_t kTotalPages = kPrimaryPages + kSupplementalPages;
constexpr uint32_t kPrimaryBytes = kPrimaryPages * kPageSize;
constexpr uint32_t kSupplementalBytes = kSupplementalPages * kPageSize;
constexpr uint32_t kVirtualBytes = kPrimaryBytes + kSupplementalBytes;

static_assert(kPrimaryPages == 32U && kSupplementalPages == 7U &&
              kTotalPages == 39U && kVirtualBytes == 156U * 1024U,
              "KQ05 segmented logical size changed");
static_assert(storage_config::kProductRegionEnd ==
                  storage_config::kGeofenceRegionStart &&
              storage_config::kFutureBondRegionEnd ==
                  storage_config::kBaseAddress &&
              storage_config::kBaseAddress + kSupplementalBytes ==
                  0xF4000U,
              "KQ05 noncontiguous physical region alignment changed");

// Test-only joiner for *two separate FlashBackend instances*.
// A runtime design must obtain legacy-writer exclusion and role/identity
// binding from real trusted owners; booleans here are solely simulated facts.
// The eventual firmware port MUST route through the single physical
// FlashMutationGate. This file MUST NOT be compiled into the RAK image.
class SegmentedFlash final : public FlashBackend {
 public:
  SegmentedFlash(FlashBackend& product, FlashBackend& supplemental,
                 kq05_supplemental::Role expected_role,
                 uint64_t local_device_id,
                 bool exclusive_history_owner_stopped,
                 bool device_role_identity_bound)
      : product_(product), supplemental_(supplemental),
        role_(expected_role), device_id_(local_device_id),
        exclusive_(exclusive_history_owner_stopped),
        bound_(device_role_identity_bound) {}

  bool begin() override {
    if (faulted_ || pending_ != Side::kNone || !exclusive_ || !bound_ ||
        product_.hasUnreconciledMutation() ||
        supplemental_.hasUnreconciledMutation()) {
      faulted_ = true;
      return false;
    }
    ready_ = false;
    if (!product_.begin() || !supplemental_.begin() ||
        product_.hasUnreconciledMutation() ||
        supplemental_.hasUnreconciledMutation()) {
      faulted_ = true;
      return false;
    }

    // Unlike a normal single-region ObservationStore recovery, a new
    // supplemental owner MUST reject uncommitted old History bytes.
    // The existing OBS1 recovery intentionally tolerates staged headers;
    // that is unsafe for an unapproved legacy-region takeover.
    // Read and classify ALL bytes of the 28 KiB region BEFORE exposing
    // even the first virtual supplemental page to a writer.
    const kq05_supplemental::Result census =
        kq05_supplemental::inspectReadOnly(
            supplemental_, role_, device_id_);
    const bool permitted =
        census.state == kq05_supplemental::State::kBlank ||
        (role_ == kq05_supplemental::Role::kTracker &&
         census.state == kq05_supplemental::State::kPotentialTracker) ||
        (role_ == kq05_supplemental::Role::kGateway &&
         bound_ &&
         census.state == kq05_supplemental::State::kGatewayNeedsBinding);
    if (!permitted) {
      faulted_ = true;
      return false;
    }
    ready_ = true;
    return true;
  }

  bool read(uint32_t offset, void* dest, size_t size) const override {
    Side side = Side::kNone;
    uint32_t translated = 0U;
    if (!ready_ || faulted_ || pending_ != Side::kNone ||
        hasUnreconciledMutation() || dest == nullptr ||
        !map(offset, size, side, translated))
      return false;
    return port(side).read(translated, dest, size);
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    Side side = Side::kNone;
    uint32_t translated = 0U;
    if (!ready_ || faulted_ || pending_ != Side::kNone ||
        hasUnreconciledMutation() || data == nullptr ||
        !map(offset, size, side, translated) ||
        ((offset % kPageSize) + size > kPageSize))
      return FlashOpResult::kFailed;
    return mutate(side, port(side).program(translated, data, size));
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (!ready_ || faulted_ || pending_ != Side::kNone ||
        hasUnreconciledMutation() || page >= kTotalPages)
      return FlashOpResult::kFailed;
    const Side side = page < kPrimaryPages
                          ? Side::kPrimary : Side::kSupplemental;
    const uint32_t local_page = page < kPrimaryPages
                                    ? page : page - kPrimaryPages;
    return mutate(side, port(side).erasePage(local_page));
  }

  FlashOpResult pollPending() override {
    if (!ready_ || faulted_ || pending_ == Side::kNone)
      return FlashOpResult::kFailed;
    const Side side = pending_;
    const auto result = port(side).pollPending();
    if (result == FlashOpResult::kPending) return result;
    pending_ = Side::kNone;
    if (result != FlashOpResult::kDone || hasUnreconciledMutation())
      faulted_ = true;
    return result;
  }

  bool hasUnreconciledMutation() const override {
    return product_.hasUnreconciledMutation() ||
           supplemental_.hasUnreconciledMutation();
  }

  bool ready() const { return ready_ && !faulted_; }
  bool pending() const { return pending_ != Side::kNone; }

 private:
  enum class Side : uint8_t { kNone, kPrimary, kSupplemental };

  // Refuse operations spanning the 128/28 KiB seam: neither physical backend
  // must ever see an out-of-bounds/partly-applied request. Store IO is
  // page-local and does not need seam-spanning calls.
  static bool map(uint32_t offset, size_t length, Side& side,
                  uint32_t& translated) {
    if (length == 0U || offset >= kVirtualBytes ||
        length > kVirtualBytes - offset) return false;
    if (offset < kPrimaryBytes) {
      if (length > kPrimaryBytes - offset) return false;
      side = Side::kPrimary;
      translated = offset;
      return true;
    }
    side = Side::kSupplemental;
    translated = offset - kPrimaryBytes;
    return true;
  }

  FlashBackend& port(Side side) const {
    return side == Side::kPrimary ? product_ : supplemental_;
  }

  FlashOpResult mutate(Side side, FlashOpResult outcome) {
    if (outcome == FlashOpResult::kPending) {
      pending_ = side;
      return outcome;
    }
    if (outcome == FlashOpResult::kFailed ||
        hasUnreconciledMutation()) {
      faulted_ = true;
      return FlashOpResult::kFailed;
    }
    return outcome;
  }

  FlashBackend& product_;
  FlashBackend& supplemental_;
  const kq05_supplemental::Role role_;
  const uint64_t device_id_;
  const bool exclusive_;
  const bool bound_;
  bool ready_ = false;
  bool faulted_ = false;
  Side pending_ = Side::kNone;
};

}  // namespace kq05_segmented_host
}  // namespace orun_tlp
