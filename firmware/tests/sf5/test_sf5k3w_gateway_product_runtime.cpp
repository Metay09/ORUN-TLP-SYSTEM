// SF5K3W: REAL GatewayCustodyAuthorizedIngress + OCQ2 fake NOR + signer,
// with a FAKE RadioManager transport. No physical RF, provisioning or CC310.
#include "gateway_product_custody_runtime.h"

#define main sf5k3s_existing_fixture_main
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

namespace {
uint32_t g_now = 1000U;
bool g_rx_pending = false;
bool g_tx_available = true;
unsigned g_ack_sends = 0U;
unsigned g_grant_denials = 0U;
uint64_t g_identity = kGateway;
orun_tlp::ProductSecureRxFrame g_rx{};
uint8_t g_ack_wire[orun_tlp::kq05_gateway_batch_ack::kMinSize]{};

void resetTransport() {
  g_now = 1000U;
  g_rx_pending = false;
  g_tx_available = true;
  g_ack_sends = 0U;
  g_grant_denials = 0U;
  g_identity = kGateway;
  g_rx = {};
  memset(g_ack_wire, 0, sizeof(g_ack_wire));
}

void queueProduct(size_t n, bool custody = true) {
  assert(!g_rx_pending && (n == 84U || n == 100U));
  g_rx = {};
  g_rx.rx_floor_ms = g_now;
  g_rx.size = static_cast<uint16_t>(n);
  frame(g_rx.payload, n, custody);
  g_rx_pending = true;
}
}  // namespace

namespace orun_tlp {
namespace monotonic { uint32_t nowMs() { return g_now; } }
bool RadioManager::canSend() const { return g_tx_available; }
uint64_t RadioManager::deviceId() const { return g_identity; }
bool RadioManager::takeProductSecureFrame(ProductSecureRxFrame* out) {
  if (!out || !g_rx_pending) return false;
  *out = g_rx;
  g_rx = {};
  g_rx_pending = false;
  return true;
}
RadioManager::BatchTx RadioManager::sendAuthorizedBatchAck(
    GatewayCustodyAuthorizedIngress& ingress,
    const ProductCustodyStore::Handle* handles, uint8_t count,
    const GatewayCustodyAckScope& scope, uint32_t heard) {

  if (!canSend()) return BatchTx::kDeferred;

  uint8_t wire[kq05_gateway_batch_ack::kMaxSize]{};

  size_t length = 0U;

  if (!ingress.prepareAuthorizedBatchAck(
          handles, count, scope, wire, length)) {
    ++g_grant_denials;
    return BatchTx::kUnverifiable;
  }

  if (!ackFitsWindow(g_now, heard, length))
    return BatchTx::kDeferred;

  assert(count == 1U && length == sizeof(g_ack_wire));
  assert(wire[1] == kq05_gateway_batch_ack::kType);

  memcpy(g_ack_wire, wire, length);

  ++g_ack_sends;

  return BatchTx::kStarted;
}
}  // namespace orun_tlp

namespace {
using Runtime = orun_tlp::GatewayProductCustodyRuntime;
void progressUntilReady(
    Runtime& runtime,
    GatewayLocalCustodyOwner& owner,
    bool enabled) {

  for (unsigned n = 0; n < 100U &&
       owner.waitingFlash(); ++n)
    runtime.update(enabled);

  assert(!owner.waitingFlash());
}

void checkCurrentAck() {
  kq05_gateway_batch_ack::Header h{};

  assert(kq05_gateway_batch_ack::decode(
      g_ack_wire, sizeof(g_ack_wire), h));

  assert(h.count == 1U);
  assert(h.tracker_device_id == kTracker);
  assert(h.gateway_device_id == kGateway);
  assert(h.key_epoch == 7U);
  checkExpected(g_ack_wire, 100U);
}

void acceptsAndRetries(bool async) {
  resetTransport();
  Nor nor;
  nor.async = async;
  ProductCustodyStore store(nor, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants grants;
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateway);
  orun_tlp::RadioManager radio;
  Runtime runtime(radio, ingress);

  queueProduct(100U);
  assert(runtime.update(false) == Runtime::Event::kNone);
  assert(g_rx_pending && nor.writes == 0 && g_ack_sends == 0);
  g_identity = kGateway + 1U;  // Wrong Gateway physical identity.
  assert(runtime.update(true) == Runtime::Event::kNone);
  assert(g_rx_pending && nor.writes == 0);
  g_identity = kGateway;

  assert(runtime.update(true) == Runtime::Event::kCustodyStarted);
  assert(!g_rx_pending && g_ack_sends == 0);
  g_tx_available = false;  // Backpressure during synchronous/async commit.
  progressUntilReady(runtime, owner, true);
  assert(g_ack_sends == 0);
  const int before = nor.writes;
  g_tx_available = true;
  g_now += 1000U;
  assert(runtime.update(true) == Runtime::Event::kAckTxStarted);
  assert(g_ack_sends == 1U);
  checkCurrentAck();
  assert(!ingress.pending());

  // Repeating the EXACT encrypted object is idempotently ACKed, no new NOR.
  queueProduct(100U);
  assert(runtime.update(true) == Runtime::Event::kDuplicateHeld);
  g_now += 1000U;
  assert(runtime.update(true) == Runtime::Event::kAckTxStarted);
  assert(nor.writes == before && g_ack_sends == 2U);
  checkCurrentAck();
}

void deniesAndRevokes() {
  resetTransport();
  Nor nor;
  nor.async = true;
  ProductCustodyStore store(nor, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants grants;
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateway);
  orun_tlp::RadioManager radio;
  Runtime runtime(radio, ingress);

  grants.enrolled = false;   // No grant must NOT even program NOR.
  queueProduct(84U);
  assert(runtime.update(true) == Runtime::Event::kDenied);
  assert(nor.writes == 0 && g_ack_sends == 0);
  grants.enrolled = true;

  queueProduct(84U);
  assert(runtime.update(true) == Runtime::Event::kCustodyStarted);
  // Even when the Gateway service stops, the already admitted flash
  // mutation is serviced to completion but emits NO unauthorized ACK.
  progressUntilReady(runtime, owner, false);
  assert(g_ack_sends == 0 && !ingress.pending());
  grants.generation = 2U;  // Revoked/rotated after durable commit.
  assert(runtime.update(true) == Runtime::Event::kNone);
  assert(!ingress.pending() && g_ack_sends == 0);
}
// Keep a real durable reference eligible, then revoke the grant.
void revokesPendingReference() {
  resetTransport();
  Nor nor;
  nor.async = true;
  ProductCustodyStore store(nor, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants grants;
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateway);
  RadioManager radio;
  Runtime runtime(radio, ingress);

  g_tx_available = false;
  queueProduct(84U);
  assert(runtime.update(true) == Runtime::Event::kCustodyStarted);
  progressUntilReady(runtime, owner, true);
  for (unsigned i = 0; i < 10U; ++i) runtime.update(true);
  assert(runtime.reservedPages() != 0U);
  assert(!ingress.pending() && g_ack_sends == 0U);
  const auto writes = nor.writes;

  grants.generation = 2U;
  g_tx_available = true;
  g_now += 1000U;
  assert(runtime.update(true) == Runtime::Event::kNone);
  assert(g_grant_denials == 1U);
  assert(g_ack_sends == 0U && nor.writes == writes);
  assert(runtime.reservedPages() != 0U);
  puts("ORUN-305 pending durable reference: revoked grant rejected PASS");
}
}  // namespace

int main() {
  revokesPendingReference();
  acceptsAndRetries(false);
  acceptsAndRetries(true);
  deniesAndRevokes();
  puts("SF5K3W Gateway real flash/HMAC + radio intake runtime: PASS (host mocked radio)");
  return 0;
}
