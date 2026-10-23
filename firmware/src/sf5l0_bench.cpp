// SF5L0 bench-only round trip with PUBLIC test keys. See sf5l0_bench.h.
// Empty translation unit unless ORUN_SF5L0_BENCH_TEST_KEYS is defined, so the
// default product image contains none of this code.
#ifdef ORUN_SF5L0_BENCH_TEST_KEYS

#include "sf5l0_bench.h"

#include <string.h>

#include "custody_ack_primitives.h"
#include "custody_store_product_format.h"
#include "product_custody_store.h"
#include "security_store.h"
#include "security_v3_format.h"

namespace orun_tlp {
namespace sf5l0_bench {

// Same public vector as the SF3 runtime qualification image.
const uint8_t kTestCredentialId[security_format::kCredentialIdSize] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
const uint8_t kTestRoot[security_format::kKRootSize] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

}  // namespace sf5l0_bench

namespace {

void wipe(void* memory, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (length--) *p++ = 0U;
}

void put32(uint32_t value, uint8_t* out) {
  out[0] = static_cast<uint8_t>(value >> 24U);
  out[1] = static_cast<uint8_t>(value >> 16U);
  out[2] = static_cast<uint8_t>(value >> 8U);
  out[3] = static_cast<uint8_t>(value);
}

void put64(uint64_t value, uint8_t* out) {
  for (unsigned i = 0U; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
}

// Must stay byte-identical to the Tracker verifier's SF5B §12 HKDF info
// (custody_ack_verifier.cpp). The SF5L0 host test checks this end to end.
constexpr char kAckInfoLabel[] = "ORUN-TLP-V2-GW-CUSTODY-ACK-v1";
constexpr size_t kInfoSize = sizeof(kAckInfoLabel) - 1U + 8U + 4U + 8U + 4U + 4U;
static_assert(kInfoSize == 57U, "SF5B custody HKDF scope changed");

}  // namespace

// ---------------------------------------------------------------- Tracker --

bool Sf5l0BenchTestProvisioning::holdsTestCredential(
    const SecurityStore& s) {
  return s.state_ == SecurityState::kProvisioned &&
         s.credential_.key_epoch == sf5l0_bench::kTestKeyEpoch &&
         memcmp(s.credential_.credential_id, sf5l0_bench::kTestCredentialId,
                security_format::kCredentialIdSize) == 0 &&
         memcmp(s.credential_.k_root, sf5l0_bench::kTestRoot,
                security_format::kKRootSize) == 0;
}

uint8_t Sf5l0BenchTestProvisioning::activePageVersion(const SecurityStore& s) {
  if (s.active_page_ < 0) return 0U;
  return s.pages_[s.active_page_].version;
}

bool Sf5l0BenchTestProvisioning::grantFor(const SecurityStore& s,
                                          uint64_t gateway_id,
                                          uint32_t& generation,
                                          uint32_t& floor) {
  if (gateway_id == 0U ||
      activePageVersion(s) != security_v3_format::kVersion)
    return false;
  for (size_t i = 0U; i < security_v3_format::kGatewayCount; ++i) {
    if (s.v3_snapshot_.gateways[i].gateway_id == gateway_id) {
      generation = s.v3_snapshot_.gateways[i].generation;
      floor = s.v3_snapshot_.custody_floor;
      return true;
    }
  }
  return false;
}

uint64_t Sf5l0BenchTestProvisioning::firstGrantedGateway(
    const SecurityStore& s) {
  if (!holdsTestCredential(s) ||
      activePageVersion(s) != security_v3_format::kVersion)
    return 0U;
  for (size_t i = 0U; i < security_v3_format::kGatewayCount; ++i)
    if (s.v3_snapshot_.gateways[i].gateway_id != 0U)
      return s.v3_snapshot_.gateways[i].gateway_id;
  return 0U;
}

void Sf5l0BenchTestProvisioning::fail(const char* why) {
  step_ = Step::kFailed;
  reason_ = why;
}

bool Sf5l0BenchTestProvisioning::start(uint64_t gateway_id) {
  if (step_ != Step::kIdle && step_ != Step::kDone &&
      step_ != Step::kFailed) {
    reason_ = "already running";
    return false;
  }
  reason_ = "";
  if (gateway_id == 0U) {
    fail("gateway id must be nonzero");
    return false;
  }
  if (!security_.ready() || security_.busy()) {
    fail("security store busy or not ready; retry");
    return false;
  }
  gateway_id_ = gateway_id;

  if (security_.state() == SecurityState::kUnprovisioned) {
    uint8_t id[security_format::kCredentialIdSize]{};
    uint8_t root[security_format::kKRootSize]{};
    memcpy(id, sf5l0_bench::kTestCredentialId, sizeof(id));
    memcpy(root, sf5l0_bench::kTestRoot, sizeof(root));
    const bool started =
        security_.commitCredential(id, sf5l0_bench::kTestKeyEpoch, root);
    wipe(root, sizeof(root));
    if (!started) {
      fail("credential commit refused");
      return false;
    }
    step_ = Step::kWaitCredential;
    return true;
  }
  if (security_.state() != SecurityState::kProvisioned) {
    fail("security store faulted; not touched");
    return false;
  }
  if (!holdsTestCredential(security_)) {
    // Never overwrite a real/unknown credential from a bench command.
    fail("device holds a different credential; not touched");
    return false;
  }
  step_ = activePageVersion(security_) == security_v3_format::kVersion
              ? Step::kGrant : Step::kMigrate;
  return true;
}

bool Sf5l0BenchTestProvisioning::poll() {
  const Step before = step_;
  switch (step_) {
    case Step::kWaitCredential: {
      bool success = false;
      if (security_.takeCommitResult(success)) {
        if (success) step_ = Step::kMigrate;
        else fail("credential commit failed");
      }
      break;
    }
    case Step::kMigrate:
      if (security_.busy()) break;
      if (activePageVersion(security_) == security_v3_format::kVersion) {
        step_ = Step::kGrant;
      } else if (security_.requestV3Migration()) {
        step_ = Step::kWaitMigration;
      } else {
        fail("v3 migration refused");
      }
      break;
    case Step::kWaitMigration:
      if (security_.busy()) break;
      if (activePageVersion(security_) == security_v3_format::kVersion &&
          security_.state() == SecurityState::kProvisioned)
        step_ = Step::kGrant;
      else
        fail("v3 migration did not complete");
      break;
    case Step::kGrant: {
      if (security_.busy()) break;
      uint32_t generation = 0U;
      uint32_t floor = 0U;
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
      if (free_slot == 0xFFU) {
        fail("no free gateway slot");
      } else if (security_.beginV3PolicyMutation(
                     SecurityStore::PolicyAction::kGrant, free_slot,
                     gateway_id_, security_.v3_snapshot_.custody_floor)) {
        step_ = Step::kWaitGrant;
      } else {
        fail("grant refused");
      }
      break;
    }
    case Step::kWaitGrant: {
      bool committed = false;
      if (security_.takeV3PolicyResult(committed)) {
        if (committed) step_ = Step::kDone;
        else fail("grant write failed");
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

const char* sf5l0StepName(Sf5l0BenchTestProvisioning::Step step) {
  switch (step) {
    case Sf5l0BenchTestProvisioning::Step::kIdle: return "IDLE";
    case Sf5l0BenchTestProvisioning::Step::kWaitCredential: return "CREDENTIAL";
    case Sf5l0BenchTestProvisioning::Step::kMigrate: return "MIGRATE_V3";
    case Sf5l0BenchTestProvisioning::Step::kWaitMigration: return "MIGRATING";
    case Sf5l0BenchTestProvisioning::Step::kGrant: return "GRANT";
    case Sf5l0BenchTestProvisioning::Step::kWaitGrant: return "GRANTING";
    case Sf5l0BenchTestProvisioning::Step::kDone: return "DONE";
    case Sf5l0BenchTestProvisioning::Step::kFailed: return "FAILED";
  }
  return "?";
}

// ---------------------------------------------------------------- Gateway --

bool Sf5l0BenchGrantSource::verifiedForLocalGateway(uint64_t gateway_id) const {
  return gateway_id_ != 0U && gateway_id == gateway_id_;
}

bool Sf5l0BenchGrantSource::activeGrant(uint64_t tracker_id,
                                        uint32_t key_epoch,
                                        GatewayCustodyAckScope& scope,
                                        uint8_t (&scoped_key)[32]) {
  if (gateway_id_ == 0U || tracker_id == 0U ||
      key_epoch != sf5l0_bench::kTestKeyEpoch) {
    ++grants_refused_;
    return false;
  }
  GatewayCustodyAckScope candidate{};
  candidate.tracker_device_id = tracker_id;
  candidate.gateway_device_id = gateway_id_;
  candidate.key_epoch = key_epoch;
  candidate.custody_floor = sf5l0_bench::kTestCustodyFloor;
  candidate.grant_generation = sf5l0_bench::kTestGrantGeneration;

  uint8_t info[kInfoSize]{};
  size_t n = 0U;
  memcpy(info, kAckInfoLabel, sizeof(kAckInfoLabel) - 1U);
  n += sizeof(kAckInfoLabel) - 1U;
  put64(candidate.tracker_device_id, info + n);
  n += 8U;
  put32(candidate.key_epoch, info + n);
  n += 4U;
  put64(candidate.gateway_device_id, info + n);
  n += 8U;
  put32(candidate.custody_floor, info + n);
  n += 4U;
  put32(candidate.grant_generation, info + n);

  uint8_t salt[16]{};
  uint8_t root[32]{};
  uint8_t key[32]{};
  memcpy(salt, sf5l0_bench::kTestCredentialId, sizeof(salt));
  memcpy(root, sf5l0_bench::kTestRoot, sizeof(root));
  const bool derived =
      custody_crypto::hkdfSha256(salt, root, info, sizeof(info), key);
  wipe(root, sizeof(root));
  wipe(salt, sizeof(salt));
  wipe(info, sizeof(info));
  if (!derived) {
    wipe(key, sizeof(key));
    ++grants_refused_;
    return false;
  }
  memcpy(scoped_key, key, sizeof(key));
  wipe(key, sizeof(key));
  scope = candidate;
  ++grants_issued_;
  return true;
}

// ------------------------------------------------------- Gateway hand-out --

uint16_t sf5l0ForEachHeld(const FlashBackend& port,
                          const ProductCustodyStore& store,
                          uint16_t page_count, Sf5l0HeldVisitor& visitor) {
  namespace fmt = custody_store_product_format;
  if (page_count == 0U || page_count > 32U) return 0U;
  struct Active { uint16_t page; uint64_t generation; };
  Active active[32]{};
  uint16_t active_count = 0U;
  for (uint16_t page = 0U; page < page_count; ++page) {
    uint8_t header[fmt::kPageHeaderSize]{};
    fmt::PageInspection inspection{};
    if (!port.read(uint32_t(page) * fmt::kPageSize, header, sizeof(header)) ||
        !fmt::inspectPageHeader(header, sizeof(header), inspection) ||
        inspection.evidence != fmt::PageEvidence::kActive)
      continue;
    // Insertion sort by generation: oldest custody first.
    uint16_t at = active_count++;
    while (at > 0U && active[at - 1U].generation > inspection.generation) {
      active[at] = active[at - 1U];
      --at;
    }
    active[at] = Active{page, inspection.generation};
  }
  uint16_t visited = 0U;
  for (uint16_t i = 0U; i < active_count; ++i) {
    for (uint16_t slot = 0U; slot < fmt::kRecordsPerPage; ++slot) {
      ProductCustodyStore::Handle handle{};
      handle.page = active[i].page;
      handle.slot = slot;
      handle.generation = active[i].generation;
      Sf5l0HeldRecord record{};
      size_t size = 0U;
      if (store.readConfirmedHeld(handle, record.object, size) !=
              ProductCustodyStore::Lookup::kFound ||
          size == 0U || size > sizeof(record.object))
        continue;
      record.page = handle.page;
      record.slot = slot;
      record.generation = handle.generation;
      record.size = static_cast<uint8_t>(size);
      visitor.held(record);
      ++visited;
    }
  }
  return visited;
}

}  // namespace orun_tlp

#endif  // ORUN_SF5L0_BENCH_TEST_KEYS
