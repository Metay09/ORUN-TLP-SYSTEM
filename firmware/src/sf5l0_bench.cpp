// SF5L0 bench-only round trip with PUBLIC test keys. See sf5l0_bench.h.
// Empty translation unit unless ORUN_SF5L0_BENCH_TEST_KEYS is defined, so the
// default product image contains none of this code.
#ifdef ORUN_SF5L0_BENCH_TEST_KEYS

#include "sf5l0_bench.h"

#include <string.h>

#include "crc32_ieee.h"
#include "custody_ack_primitives.h"
#include "custody_store_product_format.h"
#include "edge_link.h"
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
  if (s.state_ != SecurityState::kProvisioned ||
      activePageVersion(s) != security_v3_format::kVersion)
    return 0U;
  for (size_t i = 0U; i < security_v3_format::kGatewayCount; ++i)
    if (s.v3_snapshot_.gateways[i].gateway_id != 0U)
      return s.v3_snapshot_.gateways[i].gateway_id;
  return 0U;
}

bool Sf5l0BenchTestProvisioning::holdsCredential(
    const SecurityStore& s,
    const uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t key_epoch, const uint8_t (&k_root)[security_format::kKRootSize]) {
  return s.state_ == SecurityState::kProvisioned &&
         s.credential_.key_epoch == key_epoch &&
         memcmp(s.credential_.credential_id, credential_id,
                security_format::kCredentialIdSize) == 0 &&
         memcmp(s.credential_.k_root, k_root, security_format::kKRootSize) == 0;
}

bool Sf5l0BenchTestProvisioning::credentialInfo(
    const SecurityStore& s,
    uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t& key_epoch) {
  if (s.state_ != SecurityState::kProvisioned) return false;
  memcpy(credential_id, s.credential_.credential_id,
         security_format::kCredentialIdSize);
  key_epoch = s.credential_.key_epoch;
  return true;
}

void Sf5l0BenchTestProvisioning::fail(const char* why) {
  step_ = Step::kFailed;
  reason_ = why;
}

bool Sf5l0BenchTestProvisioning::start(uint64_t gateway_id) {
  return begin(gateway_id, sf5l0_bench::kTestCredentialId,
               sf5l0_bench::kTestKeyEpoch, sf5l0_bench::kTestRoot);
}

bool Sf5l0BenchTestProvisioning::startWithCredential(
    uint64_t gateway_id,
    const uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t key_epoch,
    const uint8_t (&k_root)[security_format::kKRootSize]) {
  return begin(gateway_id, credential_id, key_epoch, k_root);
}

bool Sf5l0BenchTestProvisioning::begin(
    uint64_t gateway_id,
    const uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t key_epoch,
    const uint8_t (&k_root)[security_format::kKRootSize]) {
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
  if (key_epoch == 0U || key_epoch == 0xFFFFFFFFUL) {
    fail("invalid key epoch");
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
    memcpy(id, credential_id, sizeof(id));
    memcpy(root, k_root, sizeof(root));
    const bool started = security_.commitCredential(id, key_epoch, root);
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
  if (!holdsCredential(security_, credential_id, key_epoch, k_root)) {
    // Never overwrite a different credential from a USB command.
    fail("device holds a different credential; run the security CLEAN image first");
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

// ------------------------------------------------------------ 3A lines --

namespace {
bool hexInto(const char* text, uint8_t* out, size_t bytes) {
  for (size_t i = 0; i < 2 * bytes; ++i) {
    const char c = text[i];
    uint8_t v;
    if (c >= '0' && c <= '9') v = static_cast<uint8_t>(c - '0');
    else if (c >= 'A' && c <= 'F') v = static_cast<uint8_t>(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') v = static_cast<uint8_t>(c - 'a' + 10);
    else return false;
    out[i / 2] = (i % 2) != 0U ? static_cast<uint8_t>(out[i / 2] | v)
                               : static_cast<uint8_t>(v << 4);
  }
  return true;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p) {
  uint64_t v = 0U;
  for (size_t i = 0; i < 8U; ++i) v = (v << 8) | p[i];
  return v;
}
// "<prefix>" then space-separated hex fields of the given byte sizes.
bool splitHex(const char* line, size_t length, const char* prefix,
              const size_t* sizes, size_t count, uint8_t* out) {
  const size_t plen = strlen(prefix);
  size_t want = plen;
  for (size_t i = 0; i < count; ++i) want += 1U + 2U * sizes[i];
  if (line == nullptr || length != want || memcmp(line, prefix, plen) != 0)
    return false;
  const char* p = line + plen;
  for (size_t i = 0; i < count; ++i) {
    if (*p != ' ' || !hexInto(p + 1, out, sizes[i])) return false;
    p += 1U + 2U * sizes[i];
    out += sizes[i];
  }
  return true;
}
}  // namespace

Sf5l0LineResult sf5l0ParseProvisionLine(
    const char* line, size_t length,
    uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t& key_epoch, uint8_t (&k_root)[security_format::kKRootSize],
    uint64_t& gateway_id) {
  static const size_t kSizes[] = {16U, 4U, 32U, 8U, 4U};
  uint8_t buf[64]{};
  Sf5l0LineResult r = Sf5l0LineResult::kMalformed;
  if (splitHex(line, length, "BENCH PROVISION", kSizes, 5U, buf)) {
    if (crc32Ieee(buf, 60U) != be32(buf + 60)) {
      r = Sf5l0LineResult::kBadCrc;
    } else {
      memcpy(credential_id, buf, sizeof(credential_id));
      key_epoch = be32(buf + 16);
      memcpy(k_root, buf + 20, sizeof(k_root));
      gateway_id = be64(buf + 52);
      r = Sf5l0LineResult::kOk;
    }
  }
  wipe(buf, sizeof(buf));
  return r;
}

Sf5l0LineResult sf5l0ParseGrantLine(const char* line, size_t length,
                                    uint64_t& tracker_id, uint32_t& key_epoch,
                                    uint32_t& custody_floor,
                                    uint32_t& grant_generation,
                                    uint8_t (&scoped_key)[32]) {
  static const size_t kSizes[] = {8U, 4U, 4U, 4U, 32U, 4U};
  uint8_t buf[56]{};
  Sf5l0LineResult r = Sf5l0LineResult::kMalformed;
  if (splitHex(line, length, "BENCH GRANT", kSizes, 6U, buf)) {
    if (crc32Ieee(buf, 52U) != be32(buf + 52)) {
      r = Sf5l0LineResult::kBadCrc;
    } else {
      tracker_id = be64(buf);
      key_epoch = be32(buf + 8);
      custody_floor = be32(buf + 12);
      grant_generation = be32(buf + 16);
      memcpy(scoped_key, buf + 20, sizeof(scoped_key));
      r = Sf5l0LineResult::kOk;
    }
  }
  wipe(buf, sizeof(buf));
  return r;
}

// ------------------------------------------------------------ drain pace --

namespace {
// Wrap-safe "a is at or after b" for a 32-bit millisecond clock.
bool reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}
}  // namespace

void Sf5l0DrainPacer::begin(uint32_t now_ms) {
  stats_ = Stats{};
  awaiting_ack_ = false;
  duty_wait_counted_ = false;
  sent_at_ms_ = now_ms;
  next_allowed_ms_ = now_ms;
  last_refill_ms_ = now_ms;
  tokens_us_ = uint64_t(config_.burst_ms) * 1000U;
}

void Sf5l0DrainPacer::refill(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - last_refill_ms_;
  last_refill_ms_ = now_ms;
  // elapsed[ms] * duty_ppm / 1e6 -> ms of airtime; kept in microseconds.
  tokens_us_ += uint64_t(elapsed) * config_.duty_ppm / 1000U;
  const uint64_t cap = uint64_t(config_.burst_ms) * 1000U;
  if (tokens_us_ > cap) tokens_us_ = cap;
}

bool Sf5l0DrainPacer::mayRearm(uint32_t now_ms) {
  refill(now_ms);
  if (!enabled_) return false;
  if (awaiting_ack_) {
    if (!reached(now_ms, sent_at_ms_ + config_.ack_wait_ms)) return false;
    // No durable release in time: the same record goes again, later.
    awaiting_ack_ = false;
    ++stats_.ack_timeouts;
    next_allowed_ms_ = now_ms + config_.retry_backoff_ms;
    return false;
  }
  if (!reached(now_ms, next_allowed_ms_)) return false;
  if (tokens_us_ < uint64_t(config_.airtime_ms) * 1000U) {
    if (!duty_wait_counted_) {
      ++stats_.duty_waits;
      duty_wait_counted_ = true;
    }
    return false;
  }
  duty_wait_counted_ = false;
  return true;
}

void Sf5l0DrainPacer::onSent(uint32_t now_ms) {
  refill(now_ms);
  const uint64_t cost = uint64_t(config_.airtime_ms) * 1000U;
  tokens_us_ = tokens_us_ > cost ? tokens_us_ - cost : 0U;
  awaiting_ack_ = true;
  sent_at_ms_ = now_ms;
  ++stats_.sent;
}

void Sf5l0DrainPacer::onReleased(uint32_t now_ms) {
  awaiting_ack_ = false;
  next_allowed_ms_ = now_ms + config_.gap_after_release_ms;
  ++stats_.released;
}

// ---------------------------------------------------------------- Gateway --

bool Sf5l0BenchGrantSource::verifiedForLocalGateway(uint64_t gateway_id) const {
  return gateway_id_ != 0U && gateway_id == gateway_id_;
}

Sf5l0BenchGrantSource::AddResult Sf5l0BenchGrantSource::addGrant(
    uint64_t tracker_id, uint32_t key_epoch, uint32_t custody_floor,
    uint32_t grant_generation, const uint8_t (&scoped_key)[32]) {
  if (gateway_id_ == 0U || tracker_id == 0U || key_epoch == 0U ||
      key_epoch == 0xFFFFFFFFUL)
    return AddResult::kInvalid;
  RamGrant* slot = nullptr;
  bool replaced = false;
  for (auto& g : ram_) {
    if (g.used && g.scope.tracker_device_id == tracker_id &&
        g.scope.key_epoch == key_epoch) {
      slot = &g;
      replaced = true;
      break;
    }
  }
  if (slot == nullptr)
    for (auto& g : ram_)
      if (!g.used) { slot = &g; break; }
  if (slot == nullptr) return AddResult::kFull;
  slot->scope.tracker_device_id = tracker_id;
  slot->scope.gateway_device_id = gateway_id_;
  slot->scope.key_epoch = key_epoch;
  slot->scope.custody_floor = custody_floor;
  slot->scope.grant_generation = grant_generation;
  memcpy(slot->key, scoped_key, sizeof(slot->key));
  slot->used = true;
  return replaced ? AddResult::kReplaced : AddResult::kAdded;
}

uint8_t Sf5l0BenchGrantSource::ramGrants() const {
  uint8_t n = 0U;
  for (const auto& g : ram_) n = static_cast<uint8_t>(n + (g.used ? 1U : 0U));
  return n;
}

bool Sf5l0BenchGrantSource::activeGrant(uint64_t tracker_id,
                                        uint32_t key_epoch,
                                        GatewayCustodyAckScope& scope,
                                        uint8_t (&scoped_key)[32]) {
  if (signed_ != nullptr && gateway_id_ != 0U && tracker_id != 0U &&
      signed_->verifiedForLocalGateway(gateway_id_) &&
      signed_->activeGrant(tracker_id, key_epoch, scope, scoped_key) &&
      scope.gateway_device_id == gateway_id_ &&
      scope.tracker_device_id == tracker_id && scope.key_epoch == key_epoch) {
    ++grants_issued_;
    ++signed_issued_;
    return true;
  }
  if (gateway_id_ != 0U && tracker_id != 0U) {
    for (const auto& g : ram_) {
      if (g.used && g.scope.tracker_device_id == tracker_id &&
          g.scope.key_epoch == key_epoch) {
        scope = g.scope;
        memcpy(scoped_key, g.key, sizeof(g.key));
        ++grants_issued_;
        return true;
      }
    }
  }
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

// ------------------------------------------------------------ Edge accept --

namespace {
bool hexNibble(char c, uint8_t& v) {
  if (c >= '0' && c <= '9') { v = uint8_t(c - '0'); return true; }
  if (c >= 'A' && c <= 'F') { v = uint8_t(c - 'A' + 10); return true; }
  if (c >= 'a' && c <= 'f') { v = uint8_t(c - 'a' + 10); return true; }
  return false;
}
// "<key>=<decimal>" up to a space/end.
bool takeDecimal(const char*& p, const char* end, const char* key,
                 uint32_t limit, uint32_t& out) {
  const size_t n = strlen(key);
  if (size_t(end - p) <= n || memcmp(p, key, n) != 0) return false;
  p += n;
  uint32_t v = 0; size_t digits = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10U + uint32_t(*p - '0');
    if (v > limit || ++digits > 5U) return false;
    ++p;
  }
  if (digits == 0U || (p < end && *p != ' ')) return false;
  if (p < end) ++p;
  out = v;
  return true;
}
bool takeHex(const char*& p, const char* end, const char* key, uint8_t* out,
             size_t bytes) {
  const size_t n = strlen(key);
  if (size_t(end - p) < n + 2U * bytes || memcmp(p, key, n) != 0) return false;
  p += n;
  for (size_t i = 0; i < bytes; ++i) {
    uint8_t hi = 0, lo = 0;
    if (!hexNibble(p[0], hi) || !hexNibble(p[1], lo)) return false;
    out[i] = uint8_t((hi << 4U) | lo);
    p += 2;
  }
  if (p < end && *p != ' ') return false;
  if (p < end) ++p;
  return true;
}
bool sameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;  // constant time
  for (size_t i = 0; i < n; ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
  return diff == 0U;
}
}  // namespace

bool Sf5l0EdgeHandoff::mac(uint64_t gateway_id, uint16_t page, uint16_t slot,
                           uint64_t generation, const uint8_t* object,
                           size_t size, uint8_t (&out)[32]) {
  if (object == nullptr || size == 0U) return false;
  // Fixed sizes on purpose: the CC310 custody primitives accept exactly the
  // custody-ACK lengths (HKDF info 57 B, HMAC message 102 B). Zero padding
  // after distinct labels keeps the domains separate without widening them.
  static const char kKeyInfo[] = "ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1";
  static const char kLabel[] = "ORUN-EDGE-ACCEPT-v1";
  static_assert(sizeof(kKeyInfo) - 1U + 8U <= 57U, "info fits");
  static_assert(sizeof(kLabel) - 1U + 8U + 2U + 2U + 8U + 32U <= 102U,
                "message fits");
  uint8_t info[57]{};
  memcpy(info, kKeyInfo, sizeof(kKeyInfo) - 1U);
  put64(gateway_id, info + sizeof(kKeyInfo) - 1U);
  uint8_t salt[16]{};
  uint8_t root[32]{};
  uint8_t key[32]{};
  memcpy(salt, sf5l0_bench::kTestCredentialId, sizeof(salt));
  memcpy(root, sf5l0_bench::kTestRoot, sizeof(root));
  bool ok = custody_crypto::hkdfSha256(salt, root, info, sizeof(info), key);
  wipe(root, sizeof(root));
  uint8_t message[102]{};
  size_t n = 0;
  memcpy(message, kLabel, sizeof(kLabel) - 1U);
  n += sizeof(kLabel) - 1U;
  put64(gateway_id, message + n); n += 8U;
  message[n++] = uint8_t(page >> 8U); message[n++] = uint8_t(page);
  message[n++] = uint8_t(slot >> 8U); message[n++] = uint8_t(slot);
  put64(generation, message + n); n += 8U;
  uint8_t digest[32]{};
  ok = ok && custody_crypto::sha256(object, size, digest);
  memcpy(message + n, digest, sizeof(digest));
  ok = ok && custody_crypto::hmacSha256(key, message, sizeof(message), out);
  wipe(key, sizeof(key));
  return ok;
}

Sf5l0EdgeHandoff::Reply Sf5l0EdgeHandoff::accept(const char* line,
                                                 size_t length,
                                                 uint16_t& page,
                                                 uint16_t& slot) {
  static const char kPrefix[] = "EDGE ACCEPT v1 ";
  const size_t prefix = sizeof(kPrefix) - 1U;
  if (line == nullptr || length <= prefix || memcmp(line, kPrefix, prefix) != 0)
    return Reply::kMalformed;
  const char* p = line + prefix;
  const char* end = line + length;
  uint32_t page_v = 0, slot_v = 0;
  uint8_t gen_bytes[8]{};
  uint8_t claimed[32]{};
  if (!takeDecimal(p, end, "page=", 0xFFFFU, page_v) ||
      !takeDecimal(p, end, "slot=", 0xFFFFU, slot_v) ||
      !takeHex(p, end, "gen=", gen_bytes, sizeof(gen_bytes)) ||
      !takeHex(p, end, "mac=", claimed, sizeof(claimed)) || p != end)
    return Reply::kMalformed;
  page = uint16_t(page_v);
  slot = uint16_t(slot_v);
  uint64_t generation = 0;
  for (uint8_t b : gen_bytes) generation = (generation << 8U) | b;
  return accept(page, slot, generation, claimed);
}

Sf5l0EdgeHandoff::Reply Sf5l0EdgeHandoff::accept(uint16_t page, uint16_t slot,
                                                 uint64_t generation,
                                                 const uint8_t (&claimed)[32]) {
  if (pending_) return Reply::kBusy;
  ProductCustodyStore::Handle handle{};
  handle.page = page;
  handle.slot = slot;
  handle.generation = generation;
  uint8_t object[custody_store_product_format::kMaxObjectSize]{};
  size_t size = 0;
  const auto found = store_.readConfirmedDurable(handle, object, size);
  if (found == ProductCustodyStore::Lookup::kError)
    return store_.busy() ? Reply::kBusy : Reply::kRejected;
  if (found != ProductCustodyStore::Lookup::kFound) return Reply::kUnknown;

  uint8_t expected[32]{};
  if (!mac(gateway_id_, page, slot, handle.generation, object, size, expected))
    return Reply::kRejected;
  const bool authentic = sameBytes(expected, claimed, sizeof(expected));
  wipe(expected, sizeof(expected));
  if (!authentic) return Reply::kBadMac;

  switch (store_.requestHandoff(handle, object, size)) {
    case ProductCustodyStore::Handoff::kStarted:
      pending_ = true;
      return Reply::kPending;
    case ProductCustodyStore::Handoff::kAlreadyHandedOff:
      return Reply::kAlready;
    case ProductCustodyStore::Handoff::kNotFound:
      return Reply::kUnknown;
    case ProductCustodyStore::Handoff::kBusy:
      return Reply::kBusy;
    case ProductCustodyStore::Handoff::kExhausted:
    case ProductCustodyStore::Handoff::kRejected:
      break;
  }
  return Reply::kRejected;
}

Sf5l0EdgeHandoff::Event Sf5l0EdgeHandoff::service() {
  Event event{};
  store_.poll();
  ProductCustodyStore::Maintenance done{};
  if (store_.takeMaintenanceResult(done)) {
    event.ok = done.success;
    event.page = done.handle.page;
    event.slot = done.handle.slot;
    if (done.kind == ProductCustodyStore::Maintenance::Kind::kHandoff) {
      pending_ = false;
      event.kind = Event::Kind::kHandedOff;
      if (done.success) ++handed_off_;
    } else {
      event.kind = Event::Kind::kReclaimed;
      if (done.success) ++reclaimed_;
    }
    check_reclaim_ = done.success;  // more pages may now be reclaimable
    return event;
  }
  if (check_reclaim_ && !pending_ && store_.ready() && !store_.busy() &&
      !store_.hasOutcome()) {
    const int page = store_.reclaimablePage();
    if (page >= 0 && store_.requestReclaim(uint16_t(page))) {
      event.kind = Event::Kind::kReclaimStarted;
      event.ok = true;
      event.page = uint16_t(page);
      return event;
    }
    check_reclaim_ = false;
  }
  return event;
}

// ------------------------------------------------------- BLE Edge session --

void Sf5l0EdgeLinkSession::begin() {
  active_ = true;
  hello_pending_ = true;
  priority_pending_ = true;
  scan_pending_ = true;
  dump_end_pending_ = true;
  cursor_valid_ = false;
  sent_this_pass_ = 0;
  accept_pending_ = false;
  results_n_ = 0;
}

bool Sf5l0EdgeLinkSession::pushResult(const Result& r) {
  if (results_n_ >= sizeof(results_) / sizeof(results_[0])) return false;
  results_[results_n_++] = r;
  return true;
}

void Sf5l0EdgeLinkSession::onMessage(const uint8_t* m, size_t n) {
  if (!active_ || m == nullptr || n == 0U) return;
  if (m[0] == uint8_t(edge_link::Type::kDump) && n == 1U) {
    priority_pending_ = true;
    scan_pending_ = true;           // full pass again from the start
    dump_end_pending_ = true;
    cursor_valid_ = false;
    sent_this_pass_ = 0;
    return;
  }
  edge_link::Accept a{};
  if (!edge_link::decodeAccept(m, n, a)) {
    (void)pushResult(Result{0, 0, uint8_t(edge_link::AcceptCode::kMalformed),
                            false});
    return;
  }
  edge_link::AcceptCode code = edge_link::AcceptCode::kRejected;
  switch (handoff_.accept(a.page, a.slot, a.generation, a.mac)) {
    case Sf5l0EdgeHandoff::Reply::kPending:
      accept_pending_ = true;
      accept_page_ = a.page;
      accept_slot_ = a.slot;
      return;  // onAcceptDone() answers
    case Sf5l0EdgeHandoff::Reply::kAlready: code = edge_link::AcceptCode::kAlready; break;
    case Sf5l0EdgeHandoff::Reply::kUnknown: code = edge_link::AcceptCode::kUnknown; break;
    case Sf5l0EdgeHandoff::Reply::kBusy: code = edge_link::AcceptCode::kBusy; break;
    case Sf5l0EdgeHandoff::Reply::kBadMac: code = edge_link::AcceptCode::kBadMac; break;
    case Sf5l0EdgeHandoff::Reply::kMalformed: code = edge_link::AcceptCode::kMalformed; break;
    case Sf5l0EdgeHandoff::Reply::kRejected: break;
  }
  (void)pushResult(Result{a.page, a.slot, uint8_t(code), false});
}

void Sf5l0EdgeLinkSession::onAcceptDone(uint16_t page, uint16_t slot, bool ok) {
  if (!accept_pending_ || page != accept_page_ || slot != accept_slot_) return;
  accept_pending_ = false;
  if (!active_) return;
  (void)pushResult(Result{page, slot,
                          uint8_t(ok ? edge_link::AcceptCode::kHandedOff
                                     : edge_link::AcceptCode::kFailed),
                          false});
}

void Sf5l0EdgeLinkSession::onReclaimed(uint16_t page, bool ok) {
  if (active_) (void)pushResult(Result{page, 0, uint8_t(ok ? 1U : 0U), true});
}

void Sf5l0EdgeLinkSession::onNewRecord(uint16_t page, uint16_t slot,
                                       uint64_t generation, bool has_link,
                                       int16_t rssi, int8_t snr) {
  link_valid_ = has_link;
  link_page_ = page;
  link_slot_ = slot;
  link_generation_ = generation;
  link_rssi_ = rssi;
  link_snr_ = snr;
  if (active_) {
    priority_pending_ = true;
    scan_pending_ = true;
  }
}

bool Sf5l0EdgeLinkSession::nextPriority(uint8_t* out, size_t capacity,
                                       size_t& length) {
  // The gateway cannot decrypt PRODUCT_SECURE timestamps. "Latest" here
  // means the newest *durably admitted PERIODIC record* (generation, slot),
  // NOT a fabricated claim about the record's GNSS measurement age.
  // Walking the bounded pages uses the existing read-only store API.
  struct Last : Sf5l0HeldVisitor {
    bool found = false;
    Sf5l0HeldRecord record{};
    void held(const Sf5l0HeldRecord& r) override {
      // Prioritize a location-capable PERIODIC frame, not an unrelated EVENT.
      // Bytes 0..27 are an untrusted outer header until Edge authenticates
      // the object: use its family only for scheduling, never for acceptance.
      if (r.size != 100U || r.object[3] != 0x01U) return;
      record = r;  // sf5l0ForEachHeld walks oldest -> newest
      found = true;
    }
  } last;
  (void)sf5l0ForEachHeld(port_, store_, page_count_, last);
  if (!last.found) return false;

  edge_link::Held h{};
  h.page = last.record.page;
  h.slot = last.record.slot;
  h.generation = last.record.generation;
  h.has_link = link_valid_ && h.page == link_page_ &&
               h.slot == link_slot_ && h.generation == link_generation_;
  h.rssi = link_rssi_;
  h.snr = link_snr_;
  h.size = last.record.size;
  h.object = last.record.object;
  length = edge_link::encodeHeld(h, out, capacity);
  if (length == 0U) return false;
  // Same authenticated custody bytes; a separate optional framing type
  // preserves the DUMP_END census for new and legacy phone clients.
  out[0] = uint8_t(edge_link::Type::kHeldPreview);
  return true;
}

bool Sf5l0EdgeLinkSession::nextHeld(uint8_t* out, size_t capacity,
                                    size_t& length) {
  // First held record strictly after the cursor, in (generation, slot)
  // order -- the same order sf5l0ForEachHeld visits.
  struct After : Sf5l0HeldVisitor {
    bool cursor_valid; uint64_t gen; uint16_t slot;
    bool found = false;
    Sf5l0HeldRecord record{};
    void held(const Sf5l0HeldRecord& r) override {
      if (found) return;
      if (cursor_valid && (r.generation < gen ||
                           (r.generation == gen && r.slot <= slot)))
        return;
      record = r;
      found = true;
    }
  } after;
  after.cursor_valid = cursor_valid_;
  after.gen = cursor_generation_;
  after.slot = cursor_slot_;
  (void)sf5l0ForEachHeld(port_, store_, page_count_, after);
  if (!after.found) return false;
  edge_link::Held h{};
  h.page = after.record.page;
  h.slot = after.record.slot;
  h.generation = after.record.generation;
  h.has_link = link_valid_ && h.page == link_page_ && h.slot == link_slot_ &&
               h.generation == link_generation_;
  h.rssi = link_rssi_;
  h.snr = link_snr_;
  h.size = after.record.size;
  h.object = after.record.object;
  length = edge_link::encodeHeld(h, out, capacity);
  if (length == 0U) return false;
  cursor_valid_ = true;
  cursor_generation_ = h.generation;
  cursor_slot_ = h.slot;
  ++sent_this_pass_;
  return true;
}

bool Sf5l0EdgeLinkSession::next(uint8_t* out, size_t capacity,
                                size_t& length) {
  length = 0;
  if (!active_ || out == nullptr) return false;
  if (hello_pending_) {
    struct Count : Sf5l0HeldVisitor {
      void held(const Sf5l0HeldRecord&) override {}
    } count;
    const uint16_t held = sf5l0ForEachHeld(port_, store_, page_count_, count);
    length = edge_link::encodeHello(
        gateway_id_, held,
        uint16_t(page_count_ * custody_store_product_format::kRecordsPerPage),
        out, capacity);
    hello_pending_ = false;
    return length != 0U;
  }
  if (results_n_ != 0U) {   // answers before more records
    const Result r = results_[0];
    for (uint8_t i = 1U; i < results_n_; ++i) results_[i - 1U] = results_[i];
    --results_n_;
    length = r.reclaim
        ? edge_link::encodeReclaimed(r.page, r.code != 0U, out, capacity)
        : edge_link::encodeAcceptResult(r.page, r.slot,
                                        edge_link::AcceptCode(r.code), out,
                                        capacity);
    return length != 0U;
  }
  // Latest-head lane takes exactly one message per pending event and never
  // moves the history cursor. Normal scan remains oldest first, so neither
  // history nor its custody/ACK semantics can be starved or silently skipped.
  if (priority_pending_ && store_.ready() && !store_.busy()) {
    priority_pending_ = false;
    if (nextPriority(out, capacity, length)) return true;
  }
  if (scan_pending_ && store_.ready() && !store_.busy()) {
    if (nextHeld(out, capacity, length)) return true;
    scan_pending_ = false;  // caught up until the next record arrives
  }
  if (!scan_pending_ && dump_end_pending_) {
    dump_end_pending_ = false;
    length = edge_link::encodeDumpEnd(sent_this_pass_, out, capacity);
    return length != 0U;
  }
  return false;
}

}  // namespace orun_tlp

#endif  // ORUN_SF5L0_BENCH_TEST_KEYS
