// SF5L0 bench Gateway composition. Empty unless ORUN_SF5L0_GATEWAY.
// Wires the EXISTING SF5K3P..X owners (OCQ2 custody store, local custody
// owner, authorized ingress, startup guard, custody runtime) to the radio,
// with Sf5l0BenchGrantSource as the only grant source (public test keys).
#ifdef ORUN_SF5L0_GATEWAY

#include <Arduino.h>

#include "gateway_custody_authorized_ingress.h"
#include "gateway_local_custody_owner.h"
#include "gateway_product_custody_runtime.h"
#include "gateway_product_startup_guard.h"
#include "node_role.h"
#include "product_custody_store.h"
#include "radio_manager.h"
#include "sf5l0_bench.h"
#include "storage_config.h"

namespace orun_tlp {
namespace sf5l0_gateway {
namespace {

using Guard = GatewayProductStartupGuard;
using Event = GatewayProductCustodyRuntime::Event;

FlashBackend* product_port = nullptr;
RadioManager* radio = nullptr;
ProductCustodyStore* custody = nullptr;
GatewayLocalCustodyOwner* custody_owner = nullptr;
Sf5l0BenchGrantSource* grants = nullptr;
Guard* guard = nullptr;
GatewayCustodyAuthorizedIngress* ingress = nullptr;
GatewayProductCustodyRuntime* runtime = nullptr;

uint32_t custody_started = 0;
uint32_t duplicates = 0;
uint32_t denied = 0;
uint32_t ack_tx = 0;

uint32_t erase_page = 0;
bool erase_active = false;
bool erase_waiting = false;

void hex64(char (&out)[17], uint64_t value) {
  snprintf(out, sizeof(out), "%08lX%08lX",
           static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
           static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

const char* guardName(Guard::State s) {
  switch (s) {
    case Guard::State::kNotChecked: return "NOT_CHECKED";
    case Guard::State::kServiceDisabled: return "SERVICE_DISABLED";
    case Guard::State::kInvalidIdentity: return "INVALID_IDENTITY";
    case Guard::State::kTrackerOwnsRegion: return "TRACKER_OWNS_REGION";
    case Guard::State::kUnprovisioned: return "UNPROVISIONED";
    case Guard::State::kFlashUnavailable: return "FLASH_UNAVAILABLE";
    case Guard::State::kForeignRegion: return "FOREIGN_REGION";
    case Guard::State::kNeedsPreparation: return "NEEDS_PREPARATION";
    case Guard::State::kMountRejected: return "MOUNT_REJECTED";
    case Guard::State::kReady: return "READY";
  }
  return "?";
}

bool roleOk() {
  // Continuous RX: today only the legacy BASE listen policy provides it.
  return radio != nullptr && radio->role() == NodeRole::kBase;
}

void serviceErase() {
  if (!erase_active || product_port == nullptr) return;
  FlashOpResult result;
  if (erase_waiting) {
    result = product_port->pollPending();
    if (result == FlashOpResult::kPending) return;
  } else {
    result = product_port->erasePage(erase_page);
    if (result == FlashOpResult::kPending) {
      erase_waiting = true;
      return;
    }
  }
  erase_waiting = false;
  if (result != FlashOpResult::kDone) {
    erase_active = false;
    Serial.printf("BENCH GW ERASE FAIL page=%lu\n",
                  static_cast<unsigned long>(erase_page));
    return;
  }
  if (++erase_page < storage_config::kProductRegionPages) return;
  erase_active = false;
  Serial.println(F("BENCH GW ERASE PASS; rebooting to mount"));
#if defined(NRF52_SERIES)
  Serial.flush();
  delay(200);
  NVIC_SystemReset();
#endif
}

}  // namespace

void begin(FlashBackend& port, RadioManager& radio_manager,
           uint64_t gateway_id, bool observation_store_mounted) {
  if (runtime != nullptr) return;  // once per boot
  product_port = &port;
  radio = &radio_manager;
  custody = new ProductCustodyStore(port, storage_config::kProductRegionPages);
  custody_owner = new GatewayLocalCustodyOwner(*custody);
  grants = new Sf5l0BenchGrantSource(gateway_id);
  guard = new Guard(port, storage_config::kProductRegionPages, *custody,
                    *grants, gateway_id);
  ingress = new GatewayCustodyAuthorizedIngress(*custody_owner, *grants,
                                                gateway_id);
  runtime = new GatewayProductCustodyRuntime(radio_manager, *ingress);

  const Guard::State state =
      guard->inspectAndMount(true, observation_store_mounted, gateway_id);
  char id[17];
  hex64(id, gateway_id);
  Serial.printf("BENCH GATEWAY id=%s custody_region=%s (PUBLIC TEST KEYS)\n",
                id, guardName(state));
  if (state == Guard::State::kForeignRegion ||
      state == Guard::State::kNeedsPreparation)
    Serial.println(F("BENCH GATEWAY region holds other data; send "
                     "BENCH GW ERASE PRODUCT CONFIRM to wipe it (bench board only)"));
}

void service(bool observation_store_mounted) {
  serviceErase();
  if (runtime == nullptr || guard == nullptr) return;
  const Event event = runtime->updateGuarded(
      *guard, roleOk() && !erase_active, observation_store_mounted);
  switch (event) {
    case Event::kCustodyStarted:
      ++custody_started;
      Serial.println(F("BENCH GW RX record; durable custody write started"));
      break;
    case Event::kDuplicateHeld:
      ++duplicates;
      Serial.println(F("BENCH GW RX duplicate already held; ACK deferred"));
      break;
    case Event::kDenied:
      ++denied;
      Serial.println(F("BENCH GW RX denied (no test grant for this tracker/epoch)"));
      break;
    case Event::kAckTxStarted:
      ++ack_tx;
      Serial.println(F("BENCH GW ACK TX started (after durable custody)"));
      break;
    case Event::kNone:
      break;
  }
}

void print() {
  char id[17];
  hex64(id, radio != nullptr ? radio->deviceId() : 0U);
  Serial.printf(
      "BENCH GATEWAY id=%s custody_region=%s custody_ready=%s role=%s%s "
      "custody_started=%lu duplicates=%lu denied=%lu ack_tx=%lu "
      "grants_issued=%lu grants_refused=%lu\n",
      id, guard != nullptr ? guardName(guard->state()) : "NOT_BUILT",
      custody != nullptr && custody->ready() ? "yes" : "no",
      radio != nullptr ? roleName(radio->role()) : "?",
      roleOk() ? "" : " (needs ROLE BASE for continuous RX)",
      static_cast<unsigned long>(custody_started),
      static_cast<unsigned long>(duplicates),
      static_cast<unsigned long>(denied),
      static_cast<unsigned long>(ack_tx),
      static_cast<unsigned long>(grants ? grants->grantsIssued() : 0U),
      static_cast<unsigned long>(grants ? grants->grantsRefused() : 0U));
}

void startErase() {
  if (guard == nullptr || erase_active || custody == nullptr ||
      custody->busy() || guard->state() == Guard::State::kReady) {
    Serial.println(F("BENCH GW ERASE rejected (region already READY or busy)"));
    return;
  }
  erase_page = 0;
  erase_waiting = false;
  erase_active = true;
  Serial.println(F("BENCH GW ERASE started"));
}

}  // namespace sf5l0_gateway
}  // namespace orun_tlp

#endif  // ORUN_SF5L0_GATEWAY
