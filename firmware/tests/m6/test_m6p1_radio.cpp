#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>

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

void postRxControl(bool timeout) {
  radio_driver::Guard gate;
  assert(gate);
  radio_state = RF_IDLE;
  if (timeout)
    callbacks->RxTimeout();
  else
    callbacks->RxError();
}

void sleepInitialTrackerWindow(RadioManager& manager) {
  test_now = radio_config::kWindowedRxAfterTxMs;
  manager.update(false);
  assert(sleep_calls == 1);
  const auto diagnostics = manager.listenDiagnostics();
  assert(diagnostics.listen_policy == RadioListenPolicy::kWindowed);
  assert(diagnostics.listen_state == RadioListenState::kAsleep);
}

void trackerTxDoneOpensBoundedWindow() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  const auto before = manager.listenDiagnostics();
  const uint32_t rx_before = rx_calls;
  sendLocalPosition(manager, 1);
  terminal(false);
  manager.update(false);

  auto diagnostics = manager.listenDiagnostics();
  assert(diagnostics.listen_policy == RadioListenPolicy::kWindowed);
  assert(diagnostics.listen_state == RadioListenState::kRxWindowOpen);
  assert(diagnostics.windows_opened == before.windows_opened + 1);
  assert(rx_calls == rx_before + 1);
  assert(sleep_calls == 0);

  test_now += radio_config::kWindowedRxAfterTxMs - 1;
  manager.update(false);
  assert(sleep_calls == 0);

  ++test_now;
  manager.update(false);
  diagnostics = manager.listenDiagnostics();
  assert(sleep_calls == 1);
  assert(diagnostics.sleep_entries == 1);
  assert(diagnostics.listen_state == RadioListenState::kAsleep);

  const uint32_t final_rx_calls = rx_calls;
  manager.update(false);
  manager.update(false);
  assert(sleep_calls == 1);
  assert(rx_calls == final_rx_calls);
}

void trackerTxTimeoutAlsoOpensWindow() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  sendLocalPosition(manager, 2);
  terminal(true);
  manager.update(false);
  assert(manager.txTimeouts() == 1);
  assert(manager.listenDiagnostics().listen_state ==
         RadioListenState::kRxWindowOpen);

  test_now += radio_config::kWindowedRxAfterTxMs;
  manager.update(false);
  assert(sleep_calls == 1);
  assert(manager.listenDiagnostics().listen_state == RadioListenState::kAsleep);
  manager.update(false);
  assert(sleep_calls == 1);
}

void rxActivityDoesNotExtendWindow() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  const auto initial = manager.listenDiagnostics();
  assert(initial.listen_state == RadioListenState::kRxWindowOpen);

  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0x6000000000000001ULL, 1, packet);

  test_now = 500;
  radio_state = RF_IDLE;
  rxDone(packet, sizeof(packet), -90, 5);
  const uint32_t rx_before_done = rx_calls;
  manager.update(false);
  assert(rx_calls == rx_before_done + 1);
  assert(manager.listenDiagnostics().rx_events_in_window == 1);

  test_now = 1000;
  const uint32_t rx_before_error = rx_calls;
  postRxControl(false);
  manager.update(false);
  assert(rx_calls == rx_before_error + 1);

  test_now = 2000;
  const uint32_t rx_before_timeout = rx_calls;
  postRxControl(true);
  manager.update(false);
  assert(rx_calls == rx_before_timeout + 1);

  test_now = radio_config::kWindowedRxAfterTxMs - 1;
  manager.update(false);
  assert(sleep_calls == 0);
  test_now = radio_config::kWindowedRxAfterTxMs;
  manager.update(false);
  assert(sleep_calls == 1);
}

void asleepRestoreIsIgnoredAndCounted() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  sleepInitialTrackerWindow(manager);

  const auto before = manager.listenDiagnostics();
  const uint32_t rx_before = rx_calls;
  postRxControl(true);
  manager.update(false);
  const auto after = manager.listenDiagnostics();
  assert(rx_calls == rx_before);
  assert(sleep_calls == 1);
  assert(after.stale_restores_while_asleep ==
         before.stale_restores_while_asleep + 1);
  assert(after.listen_state == RadioListenState::kAsleep);
}

void localTxWakesFromSleep() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  sleepInitialTrackerWindow(manager);

  const auto before = manager.listenDiagnostics();
  sendLocalPosition(manager, 3);
  const auto after = manager.listenDiagnostics();
  assert(send_calls == 1);
  assert(manager.isTransmitting());
  assert(after.wakes_for_tx == before.wakes_for_tx + 1);
  assert(radio_state == RF_TX_RUNNING);
}

void relayEnableFromSleepRestoresContinuousRx() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  sleepInitialTrackerWindow(manager);

  assert(manager.setRelayForwardingEnabled(true));
  auto diagnostics = manager.listenDiagnostics();
  assert(manager.relayForwardingEnabled());
  assert(diagnostics.listen_policy == RadioListenPolicy::kContinuous);
  assert(diagnostics.listen_state == RadioListenState::kRxContinuous);
  assert(radio_state == RF_RX_RUNNING);

  const uint32_t sleeps_before = sleep_calls;
  test_now += radio_config::kWindowedRxAfterTxMs * 2;
  manager.update(false);
  assert(sleep_calls == sleeps_before);
}

void roleTransitionPreventsWindowSleep() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  test_now = radio_config::kWindowedRxAfterTxMs;
  manager.setRole(NodeRole::kBase);
  manager.update(false);
  const auto diagnostics = manager.listenDiagnostics();
  assert(manager.role() == NodeRole::kBase);
  assert(diagnostics.listen_policy == RadioListenPolicy::kContinuous);
  assert(diagnostics.listen_state == RadioListenState::kRxContinuous);
  assert(radio_state == RF_RX_RUNNING);
  assert(sleep_calls == 0);
}

void noSleepWhileTxOrTransitionPending() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  sendLocalPosition(manager, 4);
  manager.setRole(NodeRole::kBase);
  test_now = radio_config::kWindowedRxAfterTxMs;
  manager.update(false);
  assert(manager.isTransmitting());
  assert(manager.role() == NodeRole::kTracker);
  assert(sleep_calls == 0);

  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());
  assert(manager.role() == NodeRole::kBase);
  assert(sleep_calls == 0);
  assert(manager.listenDiagnostics().listen_policy ==
         RadioListenPolicy::kContinuous);
}

void continuousRolesNeverUseListenSleep() {
  for (const NodeRole role : {NodeRole::kBase, NodeRole::kRelay}) {
    RadioManager manager;
    TestSequence sequences;
    beginAs(manager, sequences, role);
    assert(manager.listenDiagnostics().listen_policy ==
           RadioListenPolicy::kContinuous);

    sendLocalPosition(manager, 10);
    terminal(false);
    manager.update(false);
    postRxControl(false);
    manager.update(false);

    sendLocalPosition(manager, 11);
    terminal(true);
    manager.update(false);
    postRxControl(true);
    manager.update(false);

    test_now += radio_config::kWindowedRxAfterTxMs * 2;
    manager.update(false);
    assert(sleep_calls == 0);
    assert(manager.listenDiagnostics().listen_state ==
           RadioListenState::kRxContinuous);
    assert(radio_state == RF_RX_RUNNING);
  }
}

void deadlineWorksAcrossMonotonicWrap() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  test_now = UINT32_MAX - 1000U;
  sendLocalPosition(manager, 20);
  terminal(false);
  manager.update(false);
  assert(manager.listenDiagnostics().listen_state ==
         RadioListenState::kRxWindowOpen);

  const uint32_t wrapped_deadline =
      (UINT32_MAX - 1000U) + radio_config::kWindowedRxAfterTxMs;
  test_now = wrapped_deadline - 1U;
  manager.update(false);
  assert(sleep_calls == 0);
  test_now = wrapped_deadline;
  manager.update(false);
  assert(sleep_calls == 1);
  assert(manager.listenDiagnostics().listen_state == RadioListenState::kAsleep);
}

void rxEstimateTracksWindowTime() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  test_now = 1000;
  auto diagnostics = manager.listenDiagnostics();
  assert(diagnostics.estimated_rx_ms >= 1000);
  test_now = radio_config::kWindowedRxAfterTxMs;
  manager.update(false);
  diagnostics = manager.listenDiagnostics();
  assert(diagnostics.estimated_rx_ms >= radio_config::kWindowedRxAfterTxMs);
  assert(diagnostics.listen_state == RadioListenState::kAsleep);
}

void rxEstimateSaturatesAcrossLongContinuousRun() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kBase);
  constexpr uint32_t kStepMs = 1UL << 30;
  test_now += kStepMs; manager.update(false);
  test_now += kStepMs; manager.update(false);
  assert(manager.listenDiagnostics().estimated_rx_ms == 2UL * kStepMs);
  for (int i = 0; i < 3; ++i) { test_now += kStepMs; manager.update(false); }
  assert(manager.listenDiagnostics().estimated_rx_ms == UINT32_MAX);
  assert(radio_state == RF_RX_RUNNING && sleep_calls == 0);
}

}  // namespace

int main() {
  trackerTxDoneOpensBoundedWindow();
  trackerTxTimeoutAlsoOpensWindow();
  rxActivityDoesNotExtendWindow();
  asleepRestoreIsIgnoredAndCounted();
  localTxWakesFromSleep();
  relayEnableFromSleepRestoresContinuousRx();
  roleTransitionPreventsWindowSleep();
  noSleepWhileTxOrTransitionPending();
  continuousRolesNeverUseListenSleep();
  deadlineWorksAcrossMonotonicWrap();
  rxEstimateTracksWindowTime();
  rxEstimateSaturatesAcrossLongContinuousRun();

  puts("M6P1 RadioManager listen window checks: PASS");
}
