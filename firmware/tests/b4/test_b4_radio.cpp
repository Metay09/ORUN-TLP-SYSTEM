// Reuse the already-validated R2 fake-radio harness so B4 exercises the real
// RadioManager ownership path instead of a second, weaker mock. R2's source
// defines `int main()` without an explicit return, which is valid only for the
// language-defined main function. Renaming it for embedding turns it into an
// ordinary int function, so suppress only that artificial warning here. The R2
// suite is still compiled independently with full -Werror in run_host_tests.sh.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main r2_embedded_regression_main
#include "../r2/test_r2.cpp"
#undef main
#pragma GCC diagnostic pop

int main() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  assert(manager.role() == NodeRole::kTracker);
  assert(!manager.relayForwardingEnabled());
  assert(manager.setRelayForwardingEnabled(true));
  assert(manager.role() == NodeRole::kTracker);
  assert(manager.relayForwardingEnabled());

  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0xB400000000000001ULL, 1, packet);
  rxDone(packet, sizeof(packet), -97, 4);
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 1);
  assert(manager.relayDiagnostics().queued == 1);

  test_now = relay_config::kMaximumDelayMs;
  manager.update(false);
  assert(send_calls == 1);
  assert(manager.isTransmitting());
  assert(last_tx_size == tlp::kRelayForwardPacketSize);

  // A behavior change must not reinterpret or abort an in-flight TX.
  assert(!manager.setRelayForwardingEnabled(false));
  assert(manager.relayForwardingEnabled());
  radio_state = RF_IDLE;
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());
  assert(manager.relayDiagnostics().forwards_completed == 1);

  assert(manager.setRelayForwardingEnabled(false));
  assert(!manager.relayForwardingEnabled());
  assert(manager.role() == NodeRole::kTracker);

  makePosition(0xB400000000000002ULL, 2, packet);
  rxDone(packet, sizeof(packet), -96, 5);
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 1);
  assert(manager.relayDiagnostics().queued == 0);

  puts("B4 RadioManager independent relay behavior apply: PASS");
}
