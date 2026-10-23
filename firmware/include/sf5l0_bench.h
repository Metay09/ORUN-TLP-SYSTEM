#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gateway_product_startup_guard.h"
#include "security_format.h"

// SF5L0 — BENCH-ONLY single-record round trip with PUBLIC test keys.
//
// Purpose: prove on two real RAK4631 boards that ONE stored PERIODIC record
// travels Tracker -> Gateway, is durably held by the Gateway, an HMAC custody
// ACK returns and the Tracker durably releases that record.
//
// This is NOT production provisioning. The credential below is public (it is
// the same fixed vector the SF3 runtime qualification used); anybody can
// derive every key from it. The production enrollment path (signed OGK1
// grants, rollback anchor, SF5K3Y..AC) is unchanged and still gated.
// Compiled only when ORUN_SF5L0_BENCH_TEST_KEYS is defined, i.e. in the two
// dedicated PlatformIO bench environments and the SF5L0 host test.

namespace orun_tlp {

class SecurityStore;

namespace sf5l0_bench {

extern const uint8_t kTestCredentialId[security_format::kCredentialIdSize];
extern const uint8_t kTestRoot[security_format::kKRootSize];
constexpr uint32_t kTestKeyEpoch = 0x01020304U;
// A freshly migrated v3 SecurityStore starts at floor 0 and the first grant
// receives generation 1. The Gateway test source assumes exactly that.
constexpr uint32_t kTestCustodyFloor = 0U;
constexpr uint32_t kTestGrantGeneration = 1U;

}  // namespace sf5l0_bench

// Tracker side: one explicit USB-started sequence on the REAL SecurityStore:
//   UNPROVISIONED -> commit PUBLIC test credential -> v2->v3 migration ->
//   durable v3 grant for exactly one Gateway ID.
// An already provisioned store is touched only if it holds this same public
// test credential; any other credential is refused, never replaced.
class Sf5l0BenchTestProvisioning final {
 public:
  enum class Step : uint8_t {
    kIdle, kWaitCredential, kMigrate, kWaitMigration,
    kGrant, kWaitGrant, kDone, kFailed
  };

  explicit Sf5l0BenchTestProvisioning(SecurityStore& security)
      : security_(security) {}

  // false = refused immediately; reason() says why.
  bool start(uint64_t gateway_id);
  // Advance at most one step. true when step() changed during this call.
  bool poll();

  Step step() const { return step_; }
  const char* reason() const { return reason_; }
  uint64_t gatewayId() const { return gateway_id_; }

  // Read-only status for the BENCH? report.
  static bool holdsTestCredential(const SecurityStore& security);
  static uint8_t activePageVersion(const SecurityStore& security);
  static bool grantFor(const SecurityStore& security, uint64_t gateway_id,
                       uint32_t& generation, uint32_t& floor);
  // First granted Gateway ID on a v3 store holding the test credential, or 0.
  static uint64_t firstGrantedGateway(const SecurityStore& security);

 private:
  void fail(const char* why);

  SecurityStore& security_;
  Step step_ = Step::kIdle;
  uint64_t gateway_id_ = 0U;
  const char* reason_ = "";
};

const char* sf5l0StepName(Sf5l0BenchTestProvisioning::Step step);

// SF5L2: bench pacing for draining the Tracker backlog with the one-record
// pilot. Decides only WHEN the pilot may be re-armed; selection, sealing,
// caching and release stay with their existing owners.
//  - after a durable release: next record after a short gap;
//  - no release within ack_wait: same record again after a back-off;
//  - token bucket on PRODUCT_SECURE airtime (rate duty_ppm, depth burst_ms)
//    bounds this sender to (burst + duty x 1 h) of airtime in any hour.
// v1 POSITION / HISTORY_SECURE airtime is NOT counted here.
class Sf5l0DrainPacer final {
 public:
  struct Config {
    uint32_t gap_after_release_ms = 3000U;
    uint32_t ack_wait_ms = 15000U;       // longer than the 10 s RX window
    uint32_t retry_backoff_ms = 60000U;  // after a missed ACK
    uint32_t airtime_ms = 2300U;         // SF11 / 125 kHz, 100-byte frame
    uint32_t duty_ppm = 60000U;          // 6 % long-run, product frames only
    uint32_t burst_ms = 60000U;          // bucket depth
  };
  struct Stats {
    uint32_t sent = 0U;
    uint32_t released = 0U;
    uint32_t ack_timeouts = 0U;
    uint32_t duty_waits = 0U;
  };

  Sf5l0DrainPacer() : config_(Config()) {}
  explicit Sf5l0DrainPacer(const Config& config) : config_(config) {}

  void begin(uint32_t now_ms);
  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  // true: the pilot may be re-armed now (caller then calls rearm()).
  bool mayRearm(uint32_t now_ms);
  void onSent(uint32_t now_ms);
  void onReleased(uint32_t now_ms);

  bool awaitingAck() const { return awaiting_ack_; }
  uint32_t tokensMs() const { return static_cast<uint32_t>(tokens_us_ / 1000U); }
  const Stats& stats() const { return stats_; }
  const Config& config() const { return config_; }

 private:
  void refill(uint32_t now_ms);

  Config config_;
  Stats stats_{};
  bool enabled_ = true;
  bool awaiting_ack_ = false;
  bool duty_wait_counted_ = false;
  uint32_t sent_at_ms_ = 0U;
  uint32_t next_allowed_ms_ = 0U;
  uint32_t last_refill_ms_ = 0U;
  uint64_t tokens_us_ = 0U;  // airtime budget in microseconds
};

// Gateway side: a durable-grant source that DERIVES the scoped custody ACK
// key from the public test root. Real Gateways must never hold a Tracker
// root; this exists only because the test root is public by construction.
class Sf5l0BenchGrantSource final : public GatewayDurableGrantSource {
 public:
  explicit Sf5l0BenchGrantSource(uint64_t local_gateway_id)
      : gateway_id_(local_gateway_id) {}

  bool verifiedForLocalGateway(uint64_t gateway_id) const override;
  bool activeGrant(uint64_t tracker_id, uint32_t key_epoch,
                   GatewayCustodyAckScope& scope,
                   uint8_t (&scoped_key)[32]) override;

  uint32_t grantsIssued() const { return grants_issued_; }
  uint32_t grantsRefused() const { return grants_refused_; }

 private:
  uint64_t gateway_id_ = 0U;
  uint32_t grants_issued_ = 0U;
  uint32_t grants_refused_ = 0U;
};

class RadioManager;
class ProductCustodyStore;

// Read-only walk over every record the Gateway currently holds (OCQ2 HELD,
// confirmed through ProductCustodyStore::readConfirmedHeld), oldest page
// generation first, then slot order. Never writes, hands off or deletes.
struct Sf5l0HeldRecord {
  uint16_t page = 0U;
  uint16_t slot = 0U;
  uint64_t generation = 0U;
  uint8_t size = 0U;
  uint8_t object[100]{};
};
class Sf5l0HeldVisitor {
 public:
  virtual ~Sf5l0HeldVisitor() = default;
  virtual void held(const Sf5l0HeldRecord& record) = 0;
};
// Returns the number of records visited; false-free: unreadable or
// non-active pages are skipped, never repaired.
uint16_t sf5l0ForEachHeld(const FlashBackend& product_port,
                          const ProductCustodyStore& store,
                          uint16_t page_count, Sf5l0HeldVisitor& visitor);

// Gateway bench image composition (src/sf5l0_bench_gateway.cpp, compiled
// only with ORUN_SF5L0_GATEWAY). Kept out of main.cpp so the default image's
// source-ownership contracts stay exactly as they are.
namespace sf5l0_gateway {
// setup(), before Bluefruit.begin(): census + OCQ2 mount via the startup guard.
void begin(FlashBackend& product_port, RadioManager& radio,
           uint64_t gateway_id, bool observation_store_mounted);
// loop(), after RadioManager::update() and only while not transmitting.
void service(bool observation_store_mounted);
// "BENCH?"
void print();
// "BENCH GW ERASE PRODUCT CONFIRM": wipe this bench board's product region
// when it is not a READY custody store. include_held=true ("BENCH GW ERASE
// HELD CONFIRM") also wipes a READY store: bench-only stand-in for the
// missing authenticated Edge accept, used once the PC ledger has the records.
void startErase(bool include_held);
// "BENCH GW DUMP": print every held record as an EDGE HELD line.
void dump();
}  // namespace sf5l0_gateway

}  // namespace orun_tlp
