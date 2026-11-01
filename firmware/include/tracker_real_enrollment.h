#pragma once
#include <stddef.h>
#include <stdint.h>

#include "security_format.h"

// Phase 2 opt-in USB-only commissioning, with NO compiled-in credentials.
// Intentionally disabled in the default firmware. The BENCH wire keywords
// preserve compatibility with the owner's existing Step 3A provision tool,
// but this module never holds or generates the old public bench test key.
namespace orun_tlp {
class SecurityStore;

enum class TrackerRealProvisionParse : uint8_t {
  kOk, kMalformed, kBadCrc,
};

TrackerRealProvisionParse parseTrackerRealProvision(
    const char* line, size_t length,
    uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t& key_epoch,
    uint8_t (&k_root)[security_format::kKRootSize],
    uint64_t& gateway_id);

class TrackerRealEnrollment final {
 public:
  enum class Step : uint8_t {
    kIdle, kWaitCredential, kMigrate, kWaitMigration, kGrant,
    kWaitGrant, kDone, kFailed,
  };

  explicit TrackerRealEnrollment(SecurityStore& security)
      : security_(security) {}

  // The caller owns the serial physical-access authorization. This method
  // only writes a blank SecurityStore, or resumes with an EXACT matching
  // credential. It never overwrites foreign, faulted or different authority.
  bool start(
      uint64_t gateway_id,
      const uint8_t (&credential_id)[security_format::kCredentialIdSize],
      uint32_t key_epoch,
      const uint8_t (&k_root)[security_format::kKRootSize]);
  bool poll();
  Step step() const { return step_; }
  const char* reason() const { return reason_; }

  static const char* stepName(Step step);
  static uint8_t activePageVersion(const SecurityStore& security);
  static uint64_t firstGrantedGateway(const SecurityStore& security);
  static bool credentialInfo(
      const SecurityStore& security,
      uint8_t (&id)[security_format::kCredentialIdSize],
      uint32_t& epoch);
  static bool grantFor(
      const SecurityStore& security, uint64_t gateway,
      uint32_t& generation, uint32_t& floor);

 private:
  void fail(const char* reason);
  static bool holdsCredential(
      const SecurityStore& security,
      const uint8_t (&id)[security_format::kCredentialIdSize],
      uint32_t epoch,
      const uint8_t (&root)[security_format::kKRootSize]);

  SecurityStore& security_;
  Step step_ = Step::kIdle;
  const char* reason_ = "";
  uint64_t gateway_id_ = 0U;
};
}  // namespace orun_tlp
