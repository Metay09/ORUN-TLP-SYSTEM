#pragma once
#include <stddef.h>
#include <stdint.h>
namespace orun_tlp {
class FlashBackend {
 public:
  virtual ~FlashBackend() = default;
  virtual bool begin() = 0;
  virtual bool read(uint32_t offset, void* data, size_t size) const = 0;
  // Success means the core cache has been flushed: never cache-only success.
  virtual bool program(uint32_t offset, const void* data, size_t size) = 0;
  virtual bool erasePage(uint32_t page) = 0;
};
class NrfHistoryFlash : public FlashBackend {
 public:
  bool begin() override;
  bool read(uint32_t offset, void* data, size_t size) const override;
  bool program(uint32_t offset, const void* data, size_t size) override;
  bool erasePage(uint32_t page) override;
 private:
  bool writable() const;
  bool ready_ = false;
};
}  // namespace orun_tlp
