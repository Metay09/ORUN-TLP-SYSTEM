// SF5K3W: REAL GatewayCustodyAuthorizedIngress + OCQ2 fake NOR + signer,
// with a FAKE RadioManager transport. No physical RF, provisioning or CC310.
#include "gateway_product_custody_runtime.h"

#define main sf5k3s_existing_fixture_main
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

namespace {
bool g_rx_pending = false;
bool g_tx_available = true;
unsigned g_ack_sends = 0U;
uint64_t g_identity = kGateway;
orun_tlp::ProductSecureRxFrame g_rx{};
uint8_t g_ack_wire[orun_tlp::custody_ack::kWireSize]{};

void resetTransport() {
  g_rx_pending = false;
  g_tx_available = true;
  g_ack_sends = 0U;
  g_identity = kGateway;
  g_rx = {};
  memset(g_ack_wire, 0, sizeof(g_ack_wire));
}

void queueProduct(size_t n, bool custody = true) {
  assert(!g_rx_pending && (n == 84U || n == 100U));
  g_rx = {};
  g_rx.size = static_cast<uint16_t>(n);
  frame(g_rx.payload, n, custody);
  g_rx_pending = true;
}
}  // namespace

namespace orun_tlp {
bool RadioManager::canSend() const { return g_tx_available; }
uint64_t RadioManager::deviceId() const { return g_identity; }
bool RadioManager::takeProductSecureFrame(ProductSecureRxFrame* out) {
  if (!out || !g_rx_pending) return false;
  *out = g_rx;
  g_rx = {};
  g_rx_pending = false;
  return true;
}
bool RadioManager::sendAuthorizedCustodyAck(
    GatewayCustodyAuthorizedIngress& ingress) {
  if (!canSend() || deviceId() == 0U ||
      ingress.localGatewayId() != deviceId()) return false;
  uint8_t candidate[custody_ack::kWireSize]{};
  if (!ingress.takeAuthorizedAck(candidate)) return false;
  custody_ack::Header header{};
  if (!custody_ack::decodeHeader(candidate, sizeof(candidate), header) ||
      !header.relay_allowed || header.gateway_device_id != deviceId())
    return false;
  memcpy(g_ack_wire, candidate, sizeof(g_ack_wire));
  ++g_ack_sends;
  return true;
}
}  // namespace orun_tlp

namespace {
using Runtime = orun_tlp::GatewayProductCustodyRuntime;
void progressUntilReady(Runtime& runtime, GatewayLocalCustodyOwner& owner,
                        bool enabled) {
  for (unsigned n = 0; n < 100U && !owner.ackReady(); ++n) {
    assert(runtime.update(enabled) == Runtime::Event::kNone);
  }
  assert(owner.ackReady() && "real NOR must commit before ACK");
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
  assert(runtime.update(true) == Runtime::Event::kAckTxStarted);
  assert(g_ack_sends == 1U);
  checkExpected(g_ack_wire, 100U);
  assert(!ingress.pending());

  // Repeating the EXACT encrypted object is idempotently ACKed, no new NOR.
  queueProduct(100U);
  assert(runtime.update(true) == Runtime::Event::kAckTxStarted);
  assert(nor.writes == before && g_ack_sends == 2U);
  checkExpected(g_ack_wire, 100U);
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
  assert(g_ack_sends == 0 && ingress.pending());
  grants.generation = 2U;  // Revoked/rotated after durable commit.
  assert(runtime.update(true) == Runtime::Event::kNone);
  assert(!ingress.pending() && g_ack_sends == 0);
}
}  // namespace

int main() {
  acceptsAndRetries(false);
  acceptsAndRetries(true);
  deniesAndRevokes();
  puts("SF5K3W Gateway real flash/HMAC + radio intake runtime: PASS (host mocked radio)");
  return 0;
}
