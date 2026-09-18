#pragma once
#include <stdint.h>
#include "flash_backend.h"
#include "storage_config.h"

namespace orun_tlp {

// M7P3 (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §9): the single owner
// of every mutating (program/erasePage) history flash call. Wraps
// NrfHistoryFlash byte-for-byte unchanged for the SoftDevice-disabled path --
// still the only path exercised by shipped firmware; BLE/SoftDevice
// enablement remains a later slice (M7P7). Adds a SoftDevice-enabled
// asynchronous path using the raw Nordic sd_flash_write/sd_flash_page_erase
// SVCs directly, independent of Bluefruit: at most one physical Nordic flash
// mutation is in flight through this gate at a time.
//
// This is a client-count-one seam today (HistoryStore). It does not
// implement, queue, or arbitrate between config/security/bond clients that
// do not exist yet (M7P5/M7P6); the ADR's priority order is not implemented
// here because there is nothing yet to arbitrate between.
class FlashMutationGate : public FlashBackend {
 public:
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override;
  FlashOpResult erasePage(uint32_t page) override;
  FlashOpResult pollPending() override;

  // Drains NRF_EVT_FLASH_OPERATION_SUCCESS/ERROR (and discards any other
  // pending SoC event) from the SoftDevice event queue via the raw sd_evt_get
  // SVC. Must be called once per cooperative loop pass so a later
  // pollPending() observes the result promptly. A no-op whenever SoftDevice
  // is disabled (checked every call; never assumes a cached prior state) --
  // in shipped M7P3 firmware, SoftDevice is never enabled, so this always
  // takes that no-op path on real hardware today. Bluefruit-independent:
  // uses only the raw Nordic SVC, never Bluefruit's own event pump.
  void pumpEvents();

  struct Diagnostics {
    uint32_t submits = 0;
    uint32_t async_accepted = 0;
    uint32_t busy_retries = 0;
    uint32_t completions_success = 0;
    uint32_t completions_error = 0;
    uint32_t timeouts = 0;
    uint32_t spurious_events = 0;
  };
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  enum class Kind : uint8_t { kNone, kProgram, kErase };

  bool softDeviceEnabled() const;
  bool timedOut(uint32_t now) const;
  FlashOpResult attemptSubmit();
  void resetInFlight();

  NrfHistoryFlash sync_backend_;
  Diagnostics diagnostics_{};
  // Set by begin(); program()/erasePage() fail closed on both the sync and
  // async paths if begin() never succeeded, matching NrfHistoryFlash's own
  // independent ready_ guard rather than relying solely on the caller
  // (HistoryStore) never invoking this backend before a successful begin().
  bool ready_ = false;

  // In-flight async request state (SoftDevice-enabled path only). One
  // request at a time; program() refuses a second submission while a prior
  // one is unresolved rather than silently starting a second mutation.
  Kind in_flight_kind_ = Kind::kNone;
  uint32_t in_flight_target_ = 0;        // program: flash address; erase: page index.
  uint32_t in_flight_started_ms_ = 0;
  bool submission_accepted_ = false;     // sd_flash_* itself returned NRF_SUCCESS.
  bool event_ready_ = false;             // pumpEvents() recorded a matching completion.
  bool event_success_ = false;

  // Owned staging copy of the caller's data: Nordic's own documentation
  // requires the source buffer to remain unmodified until the completion
  // event arrives when SoftDevice is enabled, so this backend never keeps a
  // pointer into caller-owned memory across a kPending boundary. Sized for
  // the largest blob HistoryStore ever writes (storage_config::kPageHeaderSize),
  // the same bound HistoryStore itself already uses for its own blob_ buffer.
  alignas(4) uint8_t staging_[storage_config::kPageHeaderSize]{};
  uint32_t staging_size_ = 0;
};

}  // namespace orun_tlp
