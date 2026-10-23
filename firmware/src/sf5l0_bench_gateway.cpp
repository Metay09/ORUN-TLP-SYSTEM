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

// SF5L1 hand-out: newest (generation, slot) already printed as a live line.
bool printed_any = false;
uint64_t printed_generation = 0;
uint16_t printed_slot = 0;
bool emit_pending = false;

void hex64(char (&out)[17], uint64_t value) {
  snprintf(out, sizeof(out), "%08lX%08lX",
           static_cast<unsigned long>(static_cast<uint32_t>(value >> 32)),
           static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

bool newer(const Sf5l0HeldRecord& r) {
  return !printed_any || r.generation > printed_generation ||
         (r.generation == printed_generation && r.slot > printed_slot);
}

void remember(const Sf5l0HeldRecord& r) {
  if (!newer(r)) return;
  printed_any = true;
  printed_generation = r.generation;
  printed_slot = r.slot;
}

// One versioned USB line per held record. The object is the exact opaque
// PRODUCT_SECURE frame; the Gateway never decrypts it.
void printHeld(const Sf5l0HeldRecord& r) {
  static const char kHex[] = "0123456789ABCDEF";
  char object[2U * sizeof(r.object) + 1U];
  for (uint8_t i = 0; i < r.size; ++i) {
    object[2U * i] = kHex[r.object[i] >> 4U];
    object[2U * i + 1U] = kHex[r.object[i] & 0x0FU];
  }
  object[2U * r.size] = '\0';
  char gw[17];
  char gen[17];
  hex64(gw, radio != nullptr ? radio->deviceId() : 0U);
  hex64(gen, r.generation);
  // Serial.printf formats into a 256-byte buffer in the Adafruit core; the
  // 200-char object would be cut. Print the header and the object separately.
  Serial.printf("EDGE HELD v1 gw=%s page=%u slot=%u gen=%s len=%u obj=",
                gw, static_cast<unsigned>(r.page),
                static_cast<unsigned>(r.slot), gen,
                static_cast<unsigned>(r.size));
  Serial.write(reinterpret_cast<const uint8_t*>(object), 2U * r.size);
  Serial.write('\n');
}

struct PrintAll : Sf5l0HeldVisitor {
  void held(const Sf5l0HeldRecord& r) override { printHeld(r); remember(r); }
};
struct PrintNew : Sf5l0HeldVisitor {
  void held(const Sf5l0HeldRecord& r) override {
    if (!newer(r)) return;
    printHeld(r);
    remember(r);
  }
};
struct MarkOnly : Sf5l0HeldVisitor {
  void held(const Sf5l0HeldRecord& r) override { remember(r); }
};

bool canWalk() {
  return custody != nullptr && product_port != nullptr && custody->ready() &&
         !custody->busy() && !product_port->hasUnreconciledMutation();
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
  if (state == Guard::State::kReady && canWalk()) {
    // Records held from earlier boots are handed out on BENCH GW DUMP; live
    // lines cover only records that arrive from now on.
    MarkOnly mark;
    const uint16_t held = sf5l0ForEachHeld(
        port, *custody, storage_config::kProductRegionPages, mark);
    Serial.printf("BENCH GATEWAY held=%u (BENCH GW DUMP to list)\n",
                  static_cast<unsigned>(held));
  }
}

void dump() {
  if (!canWalk()) {
    Serial.println(F("EDGE DUMP busy; retry"));
    return;
  }
  PrintAll all;
  const uint16_t count = sf5l0ForEachHeld(
      *product_port, *custody, storage_config::kProductRegionPages, all);
  Serial.printf("EDGE DUMP END count=%u\n", static_cast<unsigned>(count));
}

void service(bool observation_store_mounted) {
  serviceErase();
  if (runtime == nullptr || guard == nullptr) return;
  // Hand out a newly committed record once its flash write has settled.
  if (emit_pending && canWalk()) {
    emit_pending = false;
    PrintNew fresh;
    (void)sf5l0ForEachHeld(*product_port, *custody,
                           storage_config::kProductRegionPages, fresh);
  }
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
      emit_pending = true;
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
