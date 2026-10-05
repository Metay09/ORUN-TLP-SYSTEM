#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif
#define main r2_embedded_regression_main
#include "../r2/test_r2.cpp"
#undef main
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

void gateContentionDefersWithoutDriverEntry() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  fake_task = 2;
  radio_driver::acquire();
  fake_task = 1;
  const auto calls_before = driver_calls;
  assert(!manager.setRelayForwardingEnabled(true));
  assert(!manager.relayForwardingEnabled());
  assert(driver_calls == calls_before);

  fake_task = 2;
  radio_driver::release();
  fake_task = 1;
  const auto rx_before = rx_calls;
  assert(manager.setRelayForwardingEnabled(true));
  assert(manager.relayForwardingEnabled());
  assert(radio_state == RF_RX_RUNNING);
  assert(rx_calls > rx_before);
}

void handedOffRxDrainsUnderOldBehavior() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0xB400000000000010ULL, 10, packet);
  rxDone(packet, sizeof(packet), -95, 6);

  // The callback has handed the packet to the owner while forwarding is still
  // disabled. Enabling must drain it under that old behavior, not reinterpret
  // it as relay-eligible traffic.
  assert(manager.setRelayForwardingEnabled(true));
  assert(manager.relayForwardingEnabled());
  assert(manager.relayDiagnostics().valid_packets_received == 0);
  assert(manager.relayDiagnostics().queued == 0);

  makePosition(0xB400000000000011ULL, 11, packet);
  rxDone(packet, sizeof(packet), -94, 7);
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 1);
  assert(manager.relayDiagnostics().queued == 1);
}

void delayedOldIrqCannotCrossBehaviorEpoch() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0xB400000000000020ULL, 20, packet);
  memcpy(pending_packet, packet, sizeof(packet));
  pending_rx = true;
  IrqFired = true;

  // Pending old hardware work is dispatched/drained before the behavior epoch
  // advances, so it remains ignored under TRACKER's old forwarding-disabled
  // state.
  assert(manager.setRelayForwardingEnabled(true));
  assert(manager.relayForwardingEnabled());
  assert(!pending_rx && !IrqFired);
  assert(manager.relayDiagnostics().valid_packets_received == 0);
  assert(manager.relayDiagnostics().queued == 0);

  // A delayed semaphore/GPIO wake after quiescence has no retained chip payload
  // to relabel under the new epoch.
  IrqFired = true;
  RadioBgIrqProcess();
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 0);
  assert(manager.relayDiagnostics().queued == 0);
}

void pendingRoleTransitionDefersRelayApply() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kBase);

  manager.setRole(NodeRole::kTracker);
  assert(!manager.setRelayForwardingEnabled(true));
  assert(!manager.relayForwardingEnabled());
  manager.update(false);
  manager.update(false);
  assert(manager.role() == NodeRole::kTracker);
  assert(!manager.relayForwardingEnabled());
  assert(manager.setRelayForwardingEnabled(true));
  assert(manager.relayForwardingEnabled());
}

void relayEnabledTrackerStillSendsOwnPosition() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  assert(manager.setRelayForwardingEnabled(true));

  sendLocalPosition(manager, 30);
  assert(manager.role() == NodeRole::kTracker);
  assert(manager.relayForwardingEnabled());
  assert(last_tx_size == tlp::kPositionPacketSize);

  radio_state = RF_IDLE;
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());
  assert(manager.relayForwardingEnabled());
  assert(radio_state == RF_RX_RUNNING);
}

void makeHistoryObservationFrame(
    const RadioManager& manager,
    uint8_t (&frame)[tlp::kHistoryObservationPacketSize],
    uint64_t counter = 1) {
  tlp::HistorySecurePacket packet{};
  packet.security_context = tlp::kHistorySecurityContextDeviceD2a;
  packet.app_family = tlp::kHistoryAppFamilyObservation;
  packet.path_flags = 0;
  packet.ciphertext_len = tlp::kHistoryObservationPlaintextSize;
  packet.device_id = manager.deviceId();
  packet.key_epoch = 1;
  packet.security_counter = counter;
  packet.history_incarnation = UINT64_C(0x0102030405060708);
  for (size_t i = 0; i < packet.ciphertext_len; ++i)
    packet.ciphertext[i] = static_cast<uint8_t>(i + 1U);
  for (size_t i = 0; i < tlp::kHistorySecureTagSize; ++i)
    packet.tag[i] = static_cast<uint8_t>(0xA0U + i);
  assert(tlp::serializeHistorySecurePacket(packet, frame, sizeof(frame)));
}

void historySecureTxAndRxUseLoopOwnedRadioSeam() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  uint8_t frame[tlp::kHistoryObservationPacketSize]{};
  makeHistoryObservationFrame(manager, frame);

  assert(manager.sendHistorySecurePacket(frame, sizeof(frame)));
  assert(manager.isTransmitting());
  assert(last_tx_size == sizeof(frame));
  assert(memcmp(last_tx, frame, sizeof(frame)) == 0);
  assert(manager.eventDiagnostics().history_secure_tx_attempts == 1);

  radio_state = RF_IDLE;
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());

  rxDone(frame, sizeof(frame), -91, 5);
  manager.update(false);

  HistorySecureRxFrame received{};
  assert(manager.takeHistorySecureFrame(&received));
  assert(received.size == sizeof(frame));
  assert(received.rssi_dbm == -91);
  assert(received.snr_db == 5);
  assert(memcmp(received.payload, frame, sizeof(frame)) == 0);
  assert(!manager.takeHistorySecureFrame(&received));
  assert(manager.eventDiagnostics().history_secure_rx_queued == 1);
}

void historySecureMailboxIsBounded() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  uint8_t first[tlp::kHistoryObservationPacketSize]{};
  uint8_t second[tlp::kHistoryObservationPacketSize]{};
  makeHistoryObservationFrame(manager, first, 1);
  makeHistoryObservationFrame(manager, second, 2);

  rxDone(first, sizeof(first), -90, 4);
  rxDone(second, sizeof(second), -89, 3);
  manager.update(false);

  HistorySecureRxFrame received{};
  assert(manager.takeHistorySecureFrame(&received));
  assert(memcmp(received.payload, first, sizeof(first)) == 0);
  const auto diagnostics = manager.eventDiagnostics();
  assert(diagnostics.history_secure_rx_queued == 1);
  assert(diagnostics.history_secure_rx_drops == 1);
}

}  // namespace

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
  // Relay diagnostics are lifetime counters, not live queue depth. Disabling
  // forwarding clears pending queue state but must not erase prior evidence.
  // The disabled packet therefore leaves both cumulative counters unchanged.
  assert(manager.relayDiagnostics().valid_packets_received == 1);
  assert(manager.relayDiagnostics().queued == 1);
  assert(manager.relayDiagnostics().forwards_completed == 1);
  assert(send_calls == 1);

  gateContentionDefersWithoutDriverEntry();
  handedOffRxDrainsUnderOldBehavior();
  delayedOldIrqCannotCrossBehaviorEpoch();
  pendingRoleTransitionDefersRelayApply();
  relayEnabledTrackerStillSendsOwnPosition();
  historySecureTxAndRxUseLoopOwnedRadioSeam();
  historySecureMailboxIsBounded();

  puts("B4 RadioManager independent relay behavior apply: PASS");
}
