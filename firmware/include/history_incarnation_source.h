#pragma once
#include <stdint.h>

namespace orun_tlp {

// Supplies one fresh nonzero History observation-stream incarnation.
//
// The value is not secret. It separates logical observation namespaces across
// destructive History re-baseline/reset lifetimes so
// (DeviceIdentity, HistoryIncarnation, HistoryRecordIdentity) never silently
// aliases an older stream.
//
// This is intentionally narrower than a generic RNG abstraction.
class HistoryIncarnationSource {
 public:
  virtual ~HistoryIncarnationSource() = default;
  virtual bool generate(uint64_t& incarnation) = 0;
};

// RAK4630/nRF52840 production source.
//
// HistoryStore establishes a fresh incarnation only for a blank/currently
// re-baselined partition during boot, before Bluefruit enables SoftDevice.
// Raw RNG access therefore fails closed once SoftDevice owns SoC resources.
class NrfHistoryIncarnationSource : public HistoryIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override;
};

}  // namespace orun_tlp
