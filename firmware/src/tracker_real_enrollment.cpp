// Opt-in real-credential Tracker enrollment. No public/test key material.
// Only compiled for rak4630_phase2_tracker_real_pilot. The normal image and
// existing Gateway remain untouched. This is NOT a production release gate.
#ifdef ORUN_STEP3A_REAL_TRACKER_PROVISION
#ifdef ORUN_SF5L0_BENCH_TEST_KEYS
#error "Real Tracker enrollment must never include public bench test keys"
#endif
#include "tracker_real_enrollment.h"

#include <string.h>

#include "crc32_ieee.h"
#include "security_store.h"
#include "security_v3_format.h"

namespace orun_tlp {
namespace {
void wipe(void* dest, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(dest);
  while (len--) *p++ = 0U;
}
uint32_t be32(const uint8_t* b) {
  return (uint32_t(b[0]) << 24U) | (uint32_t(b[1]) << 16U) |
         (uint32_t(b[2]) << 8U) | uint32_t(b[3]);
}
uint64_t be64(const uint8_t* b) {
  uint64_t out = 0;
  for (size_t i = 0; i < 8U; ++i) out = (out << 8U) | b[i];
  return out;
}
bool nibble(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') out = static_cast<uint8_t>(c - '0');
  else if (c >= 'A' && c <= 'F') out = static_cast<uint8_t>(c - 'A' + 10);
  else if (c >= 'a' && c <= 'f') out = static_cast<uint8_t>(c - 'a' + 10);
  else return false;
  return true;
}
bool decodeHex(const char* text, size_t bytes, uint8_t* out) {
  for (size_t i = 0; i < bytes; ++i) {
    uint8_t high = 0, low = 0;
    if (!nibble(text[i * 2], high) || !nibble(text[i * 2 + 1], low))
      return false;
    out[i] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}
}  // namespace

TrackerRealProvisionParse parseTrackerRealProvision(
    const char* line, size_t length,
    uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t& key_epoch,
    uint8_t (&k_root)[security_format::kKRootSize],
    uint64_t& gateway_id) {
  // Step 3A exact existing wire grammar:
  // BENCH PROVISION <16B id> <4B epoch> <32B root> <8B gateway> <4B crc>
  static const char kPrefix[] = "BENCH PROVISION";
  static const size_t kSizes[] = {16U, 4U, 32U, 8U, 4U};
  constexpr size_t kBytes = 64U;
  uint8_t buf[kBytes]{};
  size_t want = sizeof(kPrefix) - 1U;
  for (size_t n : kSizes) want += 1U + 2U * n;
  if (line == nullptr || length != want ||
      memcmp(line, kPrefix, sizeof(kPrefix) - 1U) != 0)
    return TrackerRealProvisionParse::kMalformed;
  const char* cur = line + sizeof(kPrefix) - 1U;
  size_t offset = 0U;
  bool ok = true;
  for (size_t n : kSizes) {
    if (*cur++ != ' ' || !decodeHex(cur, n, buf + offset)) {
      ok = false;
      break;
    }
    cur += 2U * n;
    offset += n;
  }
  TrackerRealProvisionParse result = TrackerRealProvisionParse::kMalformed;
  if (ok && crc32Ieee(buf, 60U) != be32(buf + 60U))
    result = TrackerRealProvisionParse::kBadCrc;
  else if (ok) {
    memcpy(credential_id, buf, sizeof(credential_id));
    key_epoch = be32(buf + 16U);
    memcpy(k_root, buf + 20U, sizeof(k_root));
    gateway_id = be64(buf + 52U);
    result = TrackerRealProvisionParse::kOk;
  }
  wipe(buf, sizeof(buf));
  return result;
}

uint8_t TrackerRealEnrollment::activePageVersion(const SecurityStore& s) {
  if (s.active_page_ < 0) return 0U;
  return s.pages_[s.active_page_].version;
}
bool TrackerRealEnrollment::holdsCredential(
    const SecurityStore& s,
    const uint8_t (&id)[security_format::kCredentialIdSize],
    uint32_t epoch, const uint8_t (&root)[security_format::kKRootSize]) {
  return s.state_ == SecurityState::kProvisioned &&
         s.credential_.key_epoch == epoch &&
         memcmp(s.credential_.credential_id, id, sizeof(id)) == 0 &&
         memcmp(s.credential_.k_root, root, sizeof(root)) == 0;
}
bool TrackerRealEnrollment::credentialInfo(
    const SecurityStore& s,
    uint8_t (&id)[security_format::kCredentialIdSize], uint32_t& epoch) {
  if (s.state_ != SecurityState::kProvisioned) return false;
  memcpy(id, s.credential_.credential_id, sizeof(id));
  epoch = s.credential_.key_epoch;
  return true;
}
bool TrackerRealEnrollment::grantFor(
    const SecurityStore& s, uint64_t gateway,
    uint32_t& generation, uint32_t& floor) {
  if (gateway == 0U || s.state_ != SecurityState::kProvisioned ||
      activePageVersion(s) != security_v3_format::kVersion)
    return false;
  for (size_t i = 0U; i < security_v3_format::kGatewayCount; ++i) {
    if (s.v3_snapshot_.gateways[i].gateway_id == gateway) {
      generation = s.v3_snapshot_.gateways[i].generation;
      floor = s.v3_snapshot_.custody_floor;
      return true;
    }
  }
  return false;
}
uint64_t TrackerRealEnrollment::firstGrantedGateway(const SecurityStore& s) {
  if (s.state_ != SecurityState::kProvisioned ||
      activePageVersion(s) != security_v3_format::kVersion)
    return 0U;
  for (size_t i = 0U; i < security_v3_format::kGatewayCount; ++i)
    if (s.v3_snapshot_.gateways[i].gateway_id != 0U)
      return s.v3_snapshot_.gateways[i].gateway_id;
  return 0U;
}
void TrackerRealEnrollment::fail(const char* reason) {
  step_ = Step::kFailed;
  reason_ = reason;
}
bool TrackerRealEnrollment::start(
    uint64_t gateway,
    const uint8_t (&id)[security_format::kCredentialIdSize],
    uint32_t epoch,
    const uint8_t (&root)[security_format::kKRootSize]) {
  if (step_ != Step::kIdle && step_ != Step::kDone &&
      step_ != Step::kFailed) return false;
  step_ = Step::kIdle;
  reason_ = "";
  if (gateway == 0U || epoch == 0U || epoch == UINT32_MAX) {
    fail("invalid grant scope");
    return false;
  }
  if (!security_.ready() || security_.busy()) {
    fail("security store busy or unavailable");
    return false;
  }
  gateway_id_ = gateway;
  if (security_.state() == SecurityState::kUnprovisioned) {
    uint8_t copy_id[security_format::kCredentialIdSize]{};
    uint8_t copy_root[security_format::kKRootSize]{};
    memcpy(copy_id, id, sizeof(copy_id));
    memcpy(copy_root, root, sizeof(copy_root));
    bool accepted = security_.commitCredential(copy_id, epoch, copy_root);
    wipe(copy_root, sizeof(copy_root));
    if (!accepted) {
      fail("credential commit refused");
      return false;
    }
    step_ = Step::kWaitCredential;
    return true;
  }
  if (security_.state() != SecurityState::kProvisioned) {
    fail("foreign or faulted SecurityStore; unchanged");
    return false;
  }
  if (!holdsCredential(security_, id, epoch, root)) {
    fail("credential mismatch; refusing replacement");
    return false;
  }
  step_ = activePageVersion(security_) == security_v3_format::kVersion
              ? Step::kGrant : Step::kMigrate;
  return true;
}
bool TrackerRealEnrollment::poll() {
  const Step before = step_;
  switch (step_) {
    case Step::kWaitCredential: {
      bool ok = false;
      if (security_.takeCommitResult(ok)) {
        if (ok) step_ = Step::kMigrate;
        else fail("credential commit failed");
      }
      break;
    }
    case Step::kMigrate:
      if (security_.busy()) break;
      if (activePageVersion(security_) == security_v3_format::kVersion)
        step_ = Step::kGrant;
      else if (security_.requestV3Migration())
        step_ = Step::kWaitMigration;
      else fail("security V3 migration refused");
      break;
    case Step::kWaitMigration:
      if (security_.busy()) break;
      if (activePageVersion(security_) == security_v3_format::kVersion &&
          security_.state() == SecurityState::kProvisioned)
        step_ = Step::kGrant;
      else fail("security V3 migration failed");
      break;
    case Step::kGrant: {
      if (security_.busy()) break;
      uint32_t generation = 0, floor = 0;
      if (grantFor(security_, gateway_id_, generation, floor)) {
        step_ = Step::kDone;
        break;
      }
      uint8_t free_slot = 0xFFU;
      for (uint8_t i = 0U; i < security_v3_format::kGatewayCount; ++i) {
        if (security_.v3_snapshot_.gateways[i].gateway_id == 0U) {
          free_slot = i;
          break;
        }
      }
      if (free_slot == 0xFFU)
        fail("no available Gateway grant slot");
      else if (security_.beginV3PolicyMutation(
                   SecurityStore::PolicyAction::kGrant, free_slot,
                   gateway_id_, security_.v3_snapshot_.custody_floor))
        step_ = Step::kWaitGrant;
      else fail("Gateway grant refused");
      break;
    }
    case Step::kWaitGrant: {
      bool committed = false;
      if (security_.takeV3PolicyResult(committed)) {
        if (committed) step_ = Step::kDone;
        else fail("Gateway grant commit failed");
      }
      break;
    }
    case Step::kIdle:
    case Step::kDone:
    case Step::kFailed:
      break;
  }
  return step_ != before;
}
const char* TrackerRealEnrollment::stepName(Step s) {
  switch (s) {
    case Step::kIdle: return "IDLE";
    case Step::kWaitCredential: return "CREDENTIAL";
    case Step::kMigrate: return "MIGRATE_V3";
    case Step::kWaitMigration: return "MIGRATING";
    case Step::kGrant: return "GRANT";
    case Step::kWaitGrant: return "GRANTING";
    case Step::kDone: return "DONE";
    case Step::kFailed: return "FAILED";
  }
  return "UNKNOWN";
}
}  // namespace orun_tlp
#endif  // ORUN_STEP3A_REAL_TRACKER_PROVISION
