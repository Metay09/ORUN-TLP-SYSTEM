#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"
#include "kq05_gateway_receipt_journal_format.h"

namespace orun_tlp {

// LAB / portable owner only. Never construct using the existing History
// port while legacy HistoryStore may be mounted. No physical port or BLE
// integration is installed in this PR.
//
// Accepts only offers verified by a separate, trusted Edge/Gateway admission
// owner. This mechanical store does NOT authenticate HMAC, produce STAGED,
// confirm Tracker delivery or authorize erasure.
//
// This initial owner intentionally uses *synchronous* FlashBackend only.
// If program() returns kPending, it fails closed, makes no second write and
// must not be attached to a live SoftDevice-enabled Gateway. The next slice
// must implement safe pending/completion handling through a separately
// bounded shared FlashMutationGate port before physical installation.
class Kq05GatewayReceiptJournal final {
 public:
  enum class Mount : uint8_t {
    kEmpty,         // Both pages erased; no implicit formatting
    kReady,         // Committed page 0, optional committed page 1
    kForeign,       // Valid journal belonging to a different Gateway
    kFault          // Old History, corruption, torn header or read failure
  };
  enum class Put : uint8_t {
    kStored, kDuplicate, kFull, kConflict, kRejected, kFault
  };
  explicit Kq05GatewayReceiptJournal(FlashBackend& flash,
                                     uint64_t gateway_id)
      : flash_(flash), gateway_id_(gateway_id) {}

  Mount mountReadOnly();
  // Caller MUST have independently established an explicitly erased/owned
  // region. Never erases or migrates legacy History. Host fake Flash only.
  bool initializeErased();
  // The boolean models upstream authenticated Edge/Gateway admission in the
  // host test. It must NOT become a public production security boundary.
  Put putVerified(const uint8_t* logical_offer, size_t length,
                  bool authenticated_admission);

  bool read(size_t index,
            uint8_t (&offer)[kq05_receipt_journal::kOfferMax],
            size_t& length) const;
  size_t committed() const { return count_; }
  size_t occupied() const { return occupied_; }
  bool ready() const { return state_ == Mount::kReady; }
  Mount state() const { return state_; }

 private:
  bool scan();
  bool preparePage(uint32_t page);
  bool commitBodyAndMarker(uint32_t offset, const uint8_t* body,
                           size_t body_length, const uint8_t (&marker)[4],
                           size_t full_length);
  bool readExact(uint32_t offset, const uint8_t* expected,
                 size_t length) const;
  bool inSafeBounds(uint32_t offset, size_t length) const;
  uint32_t slotOffset(uint32_t page, uint32_t slot) const;
  bool pageAvailable(uint32_t page) const;
  bool findSourceRow(const uint8_t* offer, size_t length,
                     bool& duplicate, bool& conflict) const;

  FlashBackend& flash_;
  uint64_t gateway_id_;
  Mount state_ = Mount::kFault;
  size_t count_ = 0U;
  size_t occupied_ = 0U;
  bool page_committed_[kq05_receipt_journal::kReceiptPageCount]{};
  uint8_t slots_used_[kq05_receipt_journal::kReceiptPageCount]{};
};

}  // namespace orun_tlp
