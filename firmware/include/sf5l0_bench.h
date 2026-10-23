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
// "BENCH GW ERASE PRODUCT CONFIRM": wipe this bench board's product region.
void startErase();
// "BENCH GW DUMP": print every held record as an EDGE HELD line.
void dump();
}  // namespace sf5l0_gateway

}  // namespace orun_tlp
