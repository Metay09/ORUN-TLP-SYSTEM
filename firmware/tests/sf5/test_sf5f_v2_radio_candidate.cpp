// SF5F: real RadioManager loop under synthetic R2 SX126x model.
// Feature is compile-time opt-in and NEVER qualifies sleepy-RX rendezvous,
// Gateway enrollment, ACK HMAC or physical RF durability.
#include <assert.h>
#include <string.h>
#include <stdio.h>

#define main sf5j_prior_main
#include "test_sf5j_radio_transport.cpp"
#undef main

namespace {

void relayForwardProductAndGatewayUnwrap() {
  RadioManager relay;
  TestSequence sequences;
  beginAs(relay, sequences, NodeRole::kRelay);

  uint8_t inner[tlp::kPeriodicFrameSize]{};
  makeProduct(tlp::kProductAppFamilyPeriodic, 0x1111U, inner, sizeof(inner));
  inner[4] |= tlp::kProductPathFlagRelayAllowed;
  uint8_t exact[sizeof(inner)]{};
  memcpy(exact, inner, sizeof(inner));

  rxDone(inner, sizeof(inner), -90, 4);
  memset(inner, 0, sizeof(inner));  // callback buffer not retained
  relay.update(false);
  assert(send_calls == 0U);
  assert(relay.eventDiagnostics().v2_relay_rx_queued == 1U);
  assert(!relay.takeProductSecureFrame(nullptr));

  test_now = relay_config::kMaximumDelayMs + 1U;
  relay.update(false);
  assert(send_calls == 1U);
  assert(last_tx_size == tlp::kV2RelayForwardHeaderSize + sizeof(exact));
  assert(relay.eventDiagnostics().v2_relay_tx_attempts == 1U);
  uint8_t wire[tlp::kV2RelayForwardMaxSize]{};
  const size_t size = last_tx_size;
  memcpy(wire, last_tx, size);
  tlp::V2RelayForward decoded{};
  assert(tlp::decodeV2RelayForward(wire, size, decoded) ==
         tlp::V2RelayCodecStatus::kOk);
  assert(decoded.relay_device_id == kLocalDevice);
  assert(decoded.inner_length == sizeof(exact));
  assert(memcmp(decoded.inner, exact, sizeof(exact)) == 0);
  terminal(false);
  relay.update(false);

  RadioManager gateway;
  TestSequence gateway_sequence;
  beginAs(gateway, gateway_sequence, NodeRole::kBase);
  rxDone(wire, static_cast<uint16_t>(size), -68, 7);
  gateway.update(false);
  ProductSecureRxFrame delivered{};
  assert(gateway.takeProductSecureFrame(&delivered));
  assert(delivered.size == sizeof(exact));
  assert(memcmp(delivered.payload, exact, sizeof(exact)) == 0);
  assert(!gateway.takeProductSecureFrame(&delivered));
}

void relayForwardAckAndTrackerMailbox() {
  RadioManager relay;
  TestSequence seq;
  beginAs(relay, seq, NodeRole::kRelay);
  uint8_t ack[custody_ack::kWireSize]{};
  makeAck(ack, kLocalDevice);
  uint8_t exact[sizeof(ack)]{};
  memcpy(exact, ack, sizeof(ack));

  rxDone(ack, sizeof(ack), -95, 2);
  memset(ack, 0, sizeof(ack));
  relay.update(false);
  CustodyAckRxFrame refused{};
  assert(!relay.takeCustodyAckFrame(&refused));
  assert(relay.eventDiagnostics().v2_relay_rx_queued == 1U);
  test_now = relay_config::kMaximumDelayMs + 1U;
  relay.update(false);
  assert(send_calls == 1U);
  assert(last_tx_size == tlp::kV2RelayForwardHeaderSize + sizeof(exact));
  uint8_t wire[tlp::kV2RelayForwardMaxSize]{};
  const size_t length = last_tx_size;
  memcpy(wire, last_tx, length);
  terminal(false);
  relay.update(false);

  RadioManager tracker;
  TestSequence track_seq;
  beginAs(tracker, track_seq, NodeRole::kTracker);
  rxDone(wire, static_cast<uint16_t>(length), -82, -1);
  tracker.update(false);
  CustodyAckRxFrame recv{};
  assert(tracker.takeCustodyAckFrame(&recv));
  assert(recv.size == sizeof(exact));
  assert(memcmp(recv.payload, exact, sizeof(exact)) == 0);

  // Visible matching Tracker ID is NOT custody proof. Altered HMAC remains
  // intact and unverified in the mailbox for the independent verifier.
  assert(recv.payload[40] == exact[40]);
  assert(tracker.eventDiagnostics().v2_relay_rx_queued == 1U);
}

void noNestedRelayOrRoleLeak() {
  RadioManager relay;
  TestSequence seq;
  beginAs(relay, seq, NodeRole::kRelay);
  uint8_t inner[tlp::kPeriodicFrameSize]{};
  makeProduct(tlp::kProductAppFamilyPeriodic, 0x3333U, inner, sizeof(inner));
  inner[4] |= tlp::kProductPathFlagRelayAllowed;
  tlp::V2RelayForward env{};
  env.relay_device_id = 0x9999U;
  env.inner_length = sizeof(inner);
  memcpy(env.inner, inner, sizeof(inner));
  uint8_t wrapped[tlp::kV2RelayForwardMaxSize]{};
  size_t length = 0U;
  assert(tlp::encodeV2RelayForward(env, wrapped, sizeof(wrapped), length));

  rxDone(wrapped, static_cast<uint16_t>(length), -99, -3);
  relay.update(false);
  assert(relay.eventDiagnostics().v2_relay_rx_drops == 1U);
  test_now = relay_config::kMaximumDelayMs + 1U;
  relay.update(false);
  assert(send_calls == 0U);

  // Direct-only product never enters the relay queue.
  inner[4] = tlp::kProductPathFlagCustodyRequested;
  rxDone(inner, sizeof(inner), -90, 0);
  relay.update(false);
  assert(relay.eventDiagnostics().v2_relay_rx_drops == 2U);
  assert(send_calls == 0U);

  // A queued protected copy is erased on a role change, without pretending
  // a Tracker's durable record was delivered or released.
  inner[4] |= tlp::kProductPathFlagRelayAllowed;
  rxDone(inner, sizeof(inner), -90, 0);
  relay.update(false);
  relay.setRole(NodeRole::kBase);
  relay.update(false);
  test_now += relay_config::kMaximumDelayMs + 1U;
  relay.update(false);
  assert(send_calls == 0U);
}

}  // namespace

int main() {
  // Preserve existing real radio-owner regressions (PRODUCT/ACK direct RX,
  // role transitions, malformed frame refusal and TX ownership).
  sf5j_prior_main();
  relayForwardProductAndGatewayUnwrap();
  relayForwardAckAndTrackerMailbox();
  noNestedRelayOrRoleLeak();
  puts("SF5F V2 relay RadioManager candidate RX/TX/role isolation PASS");
  return 0;
}
