// SF5L0 bench Gateway composition. Empty unless ORUN_SF5L0_GATEWAY.
// Wires the EXISTING SF5K3P..X owners (OCQ2 custody store, local custody
// owner, authorized ingress, startup guard, custody runtime) to the radio,
// with Sf5l0BenchGrantSource as the only grant source (public test keys).
#ifdef ORUN_SF5L0_GATEWAY

#include <Arduino.h>
#include <bluefruit.h>

#include "custody_store_product_format.h"
#include "edge_link.h"
#include "monotonic_time.h"
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
Sf5l0EdgeHandoff* handoff = nullptr;
Sf5l0EdgeLinkSession* edge_session = nullptr;

// ---- SF5L6 BLE Edge GATT (bench Gateway only) ----------------------------
// Same discipline as M7P7G: the write callback (BLE task) only copies one
// bounded frame under a critical section; HVC is a counter; loop() owns the
// session, the store and every SoftDevice call. Non-blocking HVX indication,
// one frame in flight (stop-and-wait).
BLEService edge_service;
BLECharacteristic edge_to_gateway;  // WRITE (Edge -> Gateway)
BLECharacteristic edge_to_edge;     // INDICATE (Gateway -> Edge)
uint8_t edge_service_uuid[16]{};
uint8_t edge_to_gateway_uuid[16]{};
uint8_t edge_to_edge_uuid[16]{};
uint16_t edge_tx_handle = BLE_GATT_HANDLE_INVALID;
bool edge_gatt_ready = false;

constexpr uint8_t kEdgeIngressSlots = 8;
struct EdgeIngress { uint16_t conn; uint8_t len; uint8_t data[edge_link::kFrameSize]; };
EdgeIngress edge_ingress[kEdgeIngressSlots]{};
volatile uint8_t edge_ingress_head = 0;
volatile uint8_t edge_ingress_count = 0;
volatile uint32_t edge_ingress_dropped = 0;
volatile uint32_t edge_hvc_events = 0;
uint32_t edge_hvc_seen = 0;

edge_link::Fragmenter edge_out;
edge_link::Reassembler edge_in;
bool edge_in_flight = false;
uint16_t edge_conn = BLE_CONN_HANDLE_INVALID;
uint8_t edge_message_id = 0;
uint32_t edge_next_try_ms = 0;
uint32_t edge_messages_out = 0;
uint32_t edge_messages_in = 0;

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
    if (edge_session != nullptr && runtime != nullptr)
      edge_session->onNewRecord(r.page, r.slot, r.generation, true,
                                runtime->lastCustodyRssi(),
                                runtime->lastCustodySnr());
  }
};
struct MarkOnly : Sf5l0HeldVisitor {
  void held(const Sf5l0HeldRecord& r) override { remember(r); }
};
struct CountOnly : Sf5l0HeldVisitor {
  void held(const Sf5l0HeldRecord&) override {}
};

constexpr uint32_t kHeldCapacity =
    storage_config::kProductRegionPages *
    custody_store_product_format::kRecordsPerPage;

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
  handoff = new Sf5l0EdgeHandoff(*custody, gateway_id);
  edge_session = new Sf5l0EdgeLinkSession(
      port, *custody, *handoff, gateway_id, storage_config::kProductRegionPages);

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

void serviceHandoff() {
  if (handoff == nullptr || erase_active || guard == nullptr ||
      guard->state() != Guard::State::kReady)
    return;
  const Sf5l0EdgeHandoff::Event e = handoff->service();
  switch (e.kind) {
    case Sf5l0EdgeHandoff::Event::Kind::kHandedOff:
      Serial.printf("EDGE ACCEPT %s page=%u slot=%u state=HANDED_OFF\n",
                    e.ok ? "OK" : "FAIL", unsigned(e.page), unsigned(e.slot));
      if (edge_session != nullptr) edge_session->onAcceptDone(e.page, e.slot, e.ok);
      break;
    case Sf5l0EdgeHandoff::Event::Kind::kReclaimStarted:
      Serial.printf("EDGE RECLAIM page=%u started\n", unsigned(e.page));
      break;
    case Sf5l0EdgeHandoff::Event::Kind::kReclaimed:
      Serial.printf("EDGE RECLAIM page=%u %s\n", unsigned(e.page),
                    e.ok ? "OK" : "FAIL");
      if (edge_session != nullptr) edge_session->onReclaimed(e.page, e.ok);
      break;
    case Sf5l0EdgeHandoff::Event::Kind::kNone:
      break;
  }
}

void service(bool observation_store_mounted) {
  serviceErase();
  if (runtime == nullptr || guard == nullptr) return;
  serviceHandoff();
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
      Serial.println(F("BENCH GW RX denied (no test grant, or custody store "
                       "full: see BENCH? held=)"));
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
  long held = -1;
  if (canWalk()) {
    CountOnly count;
    held = sf5l0ForEachHeld(*product_port, *custody,
                            storage_config::kProductRegionPages, count);
  }
  // Two lines: each stays well under the core's 256-byte printf buffer.
  Serial.printf(
      "BENCH GATEWAY id=%s custody_region=%s custody_ready=%s held=%ld/%lu "
      "role=%s%s\n",
      id, guard != nullptr ? guardName(guard->state()) : "NOT_BUILT",
      custody != nullptr && custody->ready() ? "yes" : "no",
      held, static_cast<unsigned long>(kHeldCapacity),
      radio != nullptr ? roleName(radio->role()) : "?",
      roleOk() ? "" : " (needs ROLE BASE for continuous RX)");
  Serial.printf(
      "BENCH GATEWAY custody_started=%lu duplicates=%lu denied=%lu ack_tx=%lu "
      "grants_issued=%lu grants_refused=%lu handed_off=%lu reclaimed=%lu\n"
      "BENCH GATEWAY edge_ble=%s session=%s out=%lu in=%lu dropped=%lu\n",
      static_cast<unsigned long>(custody_started),
      static_cast<unsigned long>(duplicates),
      static_cast<unsigned long>(denied),
      static_cast<unsigned long>(ack_tx),
      static_cast<unsigned long>(grants ? grants->grantsIssued() : 0U),
      static_cast<unsigned long>(grants ? grants->grantsRefused() : 0U),
      static_cast<unsigned long>(handoff ? handoff->handedOff() : 0U),
      static_cast<unsigned long>(handoff ? handoff->reclaimed() : 0U),
      edge_gatt_ready ? "ready" : "off",
      edge_session != nullptr && edge_session->active() ? "connected" : "none",
      static_cast<unsigned long>(edge_messages_out),
      static_cast<unsigned long>(edge_messages_in),
      static_cast<unsigned long>(edge_ingress_dropped));
}

void onEdgeWrite(uint16_t conn, BLECharacteristic* chr, uint8_t* data,
                 uint16_t len) {
  (void)chr;
  taskENTER_CRITICAL();
  if (data != nullptr && len > 0U && len <= edge_link::kFrameSize &&
      edge_ingress_count < kEdgeIngressSlots) {
    EdgeIngress& slot =
        edge_ingress[(edge_ingress_head + edge_ingress_count) % kEdgeIngressSlots];
    slot.conn = conn;
    slot.len = static_cast<uint8_t>(len);
    memcpy(slot.data, data, len);
    edge_ingress_count = static_cast<uint8_t>(edge_ingress_count + 1U);
  } else {
    ++edge_ingress_dropped;  // reassembly drops the message; Edge retries
  }
  taskEXIT_CRITICAL();
}

void reverseUuid(const uint8_t (&in)[16], uint8_t (&out)[16]) {
  for (uint8_t i = 0; i < 16; ++i) out[i] = in[15U - i];
}

void endEdgeSession() {
  if (edge_session != nullptr) edge_session->end();
  edge_out.clear();
  edge_in.reset();
  edge_in_flight = false;
  edge_conn = BLE_CONN_HANDLE_INVALID;
  taskENTER_CRITICAL();
  edge_ingress_count = 0;
  edge_hvc_seen = edge_hvc_events;
  taskEXIT_CRITICAL();
}

void edgeMessage(const uint8_t* message, size_t size) {
  ++edge_messages_in;
  if (edge_session != nullptr) edge_session->onMessage(message, size);
}

void edgeAccept(const char* line, size_t length) {
  if (handoff == nullptr || erase_active || guard == nullptr ||
      guard->state() != Guard::State::kReady) {
    Serial.println(F("EDGE ACCEPT BUSY page=? slot=? (store not ready)"));
    return;
  }
  uint16_t page = 0, slot = 0;
  const auto reply = handoff->accept(line, length, page, slot);
  const char* word = "REJECTED reason=STORE";
  switch (reply) {
    case Sf5l0EdgeHandoff::Reply::kPending: return;  // result follows
    case Sf5l0EdgeHandoff::Reply::kAlready: word = "OK"; break;
    case Sf5l0EdgeHandoff::Reply::kUnknown: word = "UNKNOWN"; break;
    case Sf5l0EdgeHandoff::Reply::kBusy: word = "BUSY"; break;
    case Sf5l0EdgeHandoff::Reply::kBadMac: word = "REJECTED reason=MAC"; break;
    case Sf5l0EdgeHandoff::Reply::kMalformed:
      word = "REJECTED reason=MALFORMED"; break;
    case Sf5l0EdgeHandoff::Reply::kRejected: break;
  }
  Serial.printf("EDGE ACCEPT %s page=%u slot=%u%s\n", word, unsigned(page),
                unsigned(slot),
                reply == Sf5l0EdgeHandoff::Reply::kAlready ? " state=ALREADY" : "");
}

bool beginBle() {
  reverseUuid(edge_link::kServiceUuid128, edge_service_uuid);
  reverseUuid(edge_link::kToGatewayUuid128, edge_to_gateway_uuid);
  reverseUuid(edge_link::kToEdgeUuid128, edge_to_edge_uuid);
  edge_service.setUuid(BLEUuid(edge_service_uuid));
  if (edge_service.begin() != ERROR_NONE) return false;
  edge_to_gateway.setUuid(BLEUuid(edge_to_gateway_uuid));
  edge_to_gateway.setProperties(CHR_PROPS_WRITE);
  edge_to_gateway.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  edge_to_gateway.setMaxLen(edge_link::kFrameSize);
  edge_to_gateway.setWriteCallback(onEdgeWrite, false);  // BLE task, no heap
  if (edge_to_gateway.begin() != ERROR_NONE) return false;
  edge_to_edge.setUuid(BLEUuid(edge_to_edge_uuid));
  edge_to_edge.setProperties(CHR_PROPS_INDICATE);
  edge_to_edge.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  edge_to_edge.setMaxLen(edge_link::kFrameSize);
  if (edge_to_edge.begin() != ERROR_NONE) return false;
  edge_tx_handle = edge_to_edge.handles().value_handle;
  edge_gatt_ready = edge_tx_handle != BLE_GATT_HANDLE_INVALID;
  // Let an Edge find the service by UUID without connecting first.
  if (edge_gatt_ready) Bluefruit.ScanResponse.addService(edge_service);
  return edge_gatt_ready;
}

void onBleEvent(ble_evt_t* evt) {
  if (evt != nullptr && evt->header.evt_id == BLE_GATTS_EVT_HVC &&
      evt->evt.gatts_evt.params.hvc.handle == edge_tx_handle) {
    taskENTER_CRITICAL();
    ++edge_hvc_events;
    taskEXIT_CRITICAL();
  }
}

void pollBle(bool connected, uint16_t connection_handle, bool disconnect_event,
             uint32_t now) {
  if (!edge_gatt_ready || edge_session == nullptr || guard == nullptr ||
      guard->state() != Guard::State::kReady)
    return;
  if (edge_session->active() &&
      (disconnect_event || !connected || connection_handle != edge_conn))
    endEdgeSession();
  if (connected && !edge_session->active()) {
    endEdgeSession();
    edge_conn = connection_handle;
    edge_session->begin();
  }
  if (!edge_session->active()) return;

  edge_in.poll(now);
  taskENTER_CRITICAL();
  const uint32_t hvc = edge_hvc_events;
  taskEXIT_CRITICAL();
  if (hvc != edge_hvc_seen) {
    edge_hvc_seen = hvc;
    if (edge_in_flight) {
      edge_out.advance();
      edge_in_flight = false;
      if (!edge_out.pending()) ++edge_messages_out;
    }
  }

  for (uint8_t n = 0; n < kEdgeIngressSlots; ++n) {
    EdgeIngress frame{};
    bool have = false;
    taskENTER_CRITICAL();
    if (edge_ingress_count != 0U) {
      frame = edge_ingress[edge_ingress_head];
      edge_ingress_head =
          static_cast<uint8_t>((edge_ingress_head + 1U) % kEdgeIngressSlots);
      edge_ingress_count = static_cast<uint8_t>(edge_ingress_count - 1U);
      have = true;
    }
    taskEXIT_CRITICAL();
    if (!have) break;
    if (frame.conn != edge_conn) continue;
    if (edge_in.push(frame.data, frame.len, now) ==
        edge_link::Reassembler::Result::kComplete)
      edgeMessage(edge_in.message(), edge_in.size());
  }

  if (!edge_out.pending()) {
    uint8_t message[edge_link::kMaxMessage];
    size_t size = 0;
    if (edge_session->next(message, sizeof(message), size))
      (void)edge_out.load(message, size, ++edge_message_id);
  }
  if (!edge_out.pending() || edge_in_flight ||
      !edge_to_edge.indicateEnabled(edge_conn) ||
      !monotonic::reached(now, edge_next_try_ms))
    return;
  uint8_t frame[edge_link::kFrameSize];
  uint8_t length = 0;
  if (!edge_out.peek(frame, length)) return;
  uint16_t packet_len = length;
  ble_gatts_hvx_params_t params{};
  params.handle = edge_tx_handle;
  params.type = BLE_GATT_HVX_INDICATION;
  params.p_len = &packet_len;
  params.p_data = frame;
  const uint32_t error = sd_ble_gatts_hvx(edge_conn, &params);
  if (error == NRF_SUCCESS && packet_len == length) {
    edge_in_flight = true;
  } else if (error == NRF_ERROR_BUSY || error == NRF_ERROR_INVALID_STATE ||
             error == BLE_ERROR_GATTS_SYS_ATTR_MISSING ||
             error == NRF_ERROR_RESOURCES) {
    edge_next_try_ms = now + 25U;  // e.g. the app service's indication
  } else {
    Serial.printf("EDGE BLE indication error=0x%08lX; disconnecting\n",
                  static_cast<unsigned long>(error));
    const uint16_t failed = edge_conn;
    endEdgeSession();
    (void)Bluefruit.disconnect(failed);
  }
}

void startErase(bool include_held) {
  if (guard == nullptr || erase_active || custody == nullptr ||
      custody->busy() ||
      (!include_held && guard->state() == Guard::State::kReady)) {
    Serial.println(include_held
        ? F("BENCH GW ERASE HELD rejected (busy)")
        : F("BENCH GW ERASE rejected (region already READY or busy)"));
    return;
  }
  if (include_held)
    Serial.println(F("BENCH GW ERASE HELD: deleting every record this Gateway "
                     "holds; only do this after the PC ledger has them"));
  erase_page = 0;
  erase_waiting = false;
  erase_active = true;
  Serial.println(F("BENCH GW ERASE started"));
}

}  // namespace sf5l0_gateway
}  // namespace orun_tlp

#endif  // ORUN_SF5L0_GATEWAY
