#pragma once
#include <stddef.h>
#include <stdint.h>
namespace orun_tlp {

// M7P3: program()/erasePage() may complete asynchronously once SoftDevice is
// enabled (Nordic sd_flash_write/sd_flash_page_erase accept the request and
// report completion later via NRF_EVT_FLASH_OPERATION_SUCCESS/ERROR). kPending
// means the request was accepted or is still being retried; the caller must
// not resubmit -- it must call pollPending() on a later pass instead. A
// backend that is always synchronous (SoftDevice disabled) never returns
// kPending.
enum class FlashOpResult : uint8_t { kDone, kPending, kFailed };

class FlashBackend {
 public:
  virtual ~FlashBackend() = default;
  virtual bool begin() = 0;
  virtual bool read(uint32_t offset, void* data, size_t size) const = 0;
  // Success means programming is persistent and readback verified, not cached.
  virtual FlashOpResult program(uint32_t offset, const void* data, size_t size) = 0;
  virtual FlashOpResult erasePage(uint32_t page) = 0;
  // Only meaningful after a prior program()/erasePage() on this backend
  // returned kPending. Backends that never return kPending need not
  // override this; the base implementation fails closed (kFailed) rather
  // than silently reporting a phantom completion for a call that was never
  // actually pending.
  virtual FlashOpResult pollPending() { return FlashOpResult::kFailed; }
};
class NrfHistoryFlash : public FlashBackend {
 public:
  // M0-M7P2: synchronous Nordic API only; enabled SoftDevice is unsupported.
  // Never returns kPending. Unchanged by M7P3; FlashMutationGate (see
  // flash_mutation_gate.h) owns this instance for the SoftDevice-disabled
  // path instead of relaxing this guard.
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override;
  FlashOpResult erasePage(uint32_t page) override;
 private:
  bool ready_ = false;
};

// M7P5: the same synchronous-only Nordic primitive contract as
// NrfHistoryFlash, addressed at the M7P1-decided durable-config partition
// (storage_config::kFutureConfigRegionStart, 2 pages) instead of history's.
// A sibling backend, not a generalization of NrfHistoryFlash: no shared
// code or state (own bounds, own address arithmetic, own class). Its
// begin() does reuse NrfHistoryFlash's exact `__flash_arduino_end ==
// kBaseAddress` literal check -- that fact describes the vendor linker
// script's fixed FLASH region end (always kBaseAddress on this board,
// regardless of application size), not something specific to history, so
// it is the correct check for any partition below history, not a
// history-only invariant to avoid reusing. See nrf_config_flash.cpp's
// begin() for the full explanation. Never returns kPending;
// FlashMutationGate owns this instance for config's SoftDevice-disabled
// path exactly as it owns NrfHistoryFlash for history's.
class NrfConfigFlash : public FlashBackend {
 public:
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override;
  FlashOpResult erasePage(uint32_t page) override;
 private:
  bool ready_ = false;
};
}  // namespace orun_tlp
