#include <vector>
#include "lora_command_relay.h"
// Real radio owner and batch signer, fake DIO, monotonic clock and NOR.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main radio_owner_regressions
#include "../r2/test_r2.cpp"
#undef main
#pragma GCC diagnostic pop
#define main custody_ingress_regressions
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

namespace orun_tlp {
struct RadioBudgetHostProbe {
  static void sendTest(RadioManager& radio) {
    radio_driver::Guard gate;
    assert(gate);
    radio.sendTestPacket();
  }
};
}

namespace {
struct TimedGrants final : GatewayCustodyGrantProvider {
  Grants grants;
  uint32_t delay_ms = 0;
  bool activeGrant(uint64_t id, uint32_t epoch, GatewayCustodyAckScope& scope,
                   uint8_t (&key)[32]) override {
    test_now += delay_ms;
    return grants.activeGrant(id, epoch, scope, key);
  }
};

void checkSignedWindow(uint8_t count) {
  TimedGrants grants;
  Nor flash;
  ProductCustodyStore store(flash, 2);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateway);
  ProductCustodyStore::Handle handles[5]{};
  GatewayCustodyAckScope scope{};
  for (uint8_t i = 0; i < count; ++i) {
    uint8_t bytes[100];
    frame(bytes, sizeof(bytes));
    put64(bytes + 20, 42U + i);
    assert(ingress.receive(bytes, sizeof(bytes), true) ==
           GatewayCustodyAuthorizedIngress::Result::kStarted);
    for (unsigned n = 0; n < 150 && !ingress.batchReady(); ++n) ingress.service();
    assert(ingress.takeAuthorizedBatchHandle(handles[i], scope));
  }
  RadioManager radio;
  TestSequence sequences;
  radio.setDeviceIdentity(DeviceIdentity::fromLegacyUint64(kGateway));
  beginAs(radio, sequences, NodeRole::kBase);
  using Result = RadioManager::BatchTx;
  radio.enableGatewayBudget();
  auto sendBatch = [&](uint32_t heard) {
    return radio.sendAuthorizedBatchAck(ingress, handles, count, scope, heard);
  };
  const uint8_t size = 48U + count * 8U;
  assert(radio.gatewayAirtimeAvailable(size));
  assert(!send_calls);
  const uint32_t cutoff = 10000U - kAckTurnaroundGuardMs -
      (profileAirtimeUs(size) + 999U) / 1000U;
  const uint32_t heard = UINT32_MAX - 5000U; // deadline crosses clock wrap
  test_now = 0x7FFFFFFFU;
  assert(radio.gatewayAirtimeAvailable(size));
  test_now = heard;
  assert(radio.gatewayAirtimeAvailable(size));
  test_now += cutoff - 2U;
  grants.delay_ms = 1; // both actual grant reads consume 2 ms during preparation
  assert(sendBatch(heard) == Result::kDeferred);
  assert(!send_calls && !radio.isTransmitting());
  assert(radio.rfAirtimeUsedMs() == 0U);
  radio.update(false);
  assert(radio.canSend() && radio_state == RF_RX_RUNNING);
  grants.delay_ms = 0;
  uint32_t next = test_now + 60000U;
  test_now = next + cutoff - 1U;
  standby_cost_ms = 1;
  assert(sendBatch(next) == Result::kDeferred && !send_calls);
  standby_cost_ms = 0;
  radio.update(false);
  assert(radio.canSend() && radio_state == RF_RX_RUNNING);
  next = test_now + 60000U;
  test_now = next + cutoff - 1U;
  assert(sendBatch(next) == Result::kStarted && send_calls == 1);
  assert(sendBatch(next) == Result::kDeferred && send_calls == 1);
  assert(radio.rfAirtimeUsedMs() ==
         RfDutyBudget::costMs(size));

  printf("ORUN-201 TX BATCH_ACK count=%u cost=%lu PASS\n",
         unsigned(count),
         (unsigned long)radio.rfAirtimeUsedMs());
  kq05_gateway_batch_ack::Header decoded{};
  assert(kq05_gateway_batch_ack::decode(last_tx, last_tx_size, decoded));
  assert(decoded.count == count && decoded.tracker_device_id == kTracker);
  test_now += (profileAirtimeUs(size) + 999U) / 1000U;
  terminal(false);
  radio.update(false);
  assert(radio.eventDiagnostics().batch_ack_max_done_age_ms < 10000U);

  DownlinkTxResult outcome{};
  uint8_t cmd[54]{};
  cmd[0] = lora_command::kVersion;
  cmd[1] = lora_command::kCommandType;
  cmd[3] = lora_command::kResourceSettings;
  put64(cmd + 4, kTracker);
  cmd[19] = 99U;
  cmd[32] = 1U;
  cmd[33] = 1U;
  assert(lora_command::wellFormed(cmd, sizeof(cmd)));
  const uint32_t fresh = test_now;
  const uint32_t charged = radio.rfAirtimeUsedMs();
  assert(sendBatch(fresh) == Result::kDeferred);
  assert(!radio.sendLoraFrame(cmd, sizeof(cmd), &fresh));
  assert(send_calls == 1U && radio.rfAirtimeUsedMs() == charged);
  assert(radio.takeDownlinkTxResult(outcome));
  assert(outcome.kind == DownlinkTxResult::Kind::kAck);
  assert(outcome.target == kTracker);
  assert(outcome.rx_floor_ms == next);
  assert(outcome.succeeded);
  assert(!radio.takeDownlinkTxResult(outcome));

  // A command terminal also excludes both types of new downlink.
  assert(radio.sendLoraFrame(cmd, sizeof(cmd), &fresh));
  test_now += RfDutyBudget::costMs(sizeof(cmd));
  terminal(false);
  radio.update(false);
  const uint32_t after_command = radio.rfAirtimeUsedMs();
  assert(sendBatch(test_now) == Result::kDeferred);
  assert(!radio.sendLoraFrame(cmd, sizeof(cmd), &fresh));
  assert(send_calls == 2U && radio.rfAirtimeUsedMs() == after_command);
  assert(radio.takeDownlinkTxResult(outcome));
  assert(outcome.kind == DownlinkTxResult::Kind::kCommand);
  assert(outcome.command_id == 99U && outcome.target == kTracker && outcome.succeeded);
  assert(!radio.takeDownlinkTxResult(outcome));

  assert(sendBatch(test_now) == Result::kStarted);
  test_now += RfDutyBudget::costMs(size);
  terminal(false);
  radio.update(false);
  assert(radio.takeDownlinkTxResult(outcome));
  assert(outcome.kind == DownlinkTxResult::Kind::kAck && outcome.succeeded);
  puts("ORUN-305 P2 unread command/ACK terminals protected PASS");
}


void checkSharedBudget() {
  RfDutyBudget budget;

  assert(budget.available(0U, 100U));
  assert(budget.usedMs() == 0U);

  uint32_t now = 0U;

  for (unsigned i = 0; i < 162U; ++i) {
    assert(budget.reserve(now, 100U));
    now += 2220U;
    budget.finish(now);
  }

  assert(budget.usedMs() ==
         162U * RfDutyBudget::costMs(100U));

  assert(!budget.available(now, 56U));
  assert(!budget.reserve(now, 56U));

  // Boundary: do not drop a frame that crosses a minute.
  RfDutyBudget boundary;
  assert(boundary.observe(0U));
  assert(boundary.reserve(59999U, 100U));
  boundary.finish(62220U);

  assert(!boundary.available(3660000U, 100U));
  assert(boundary.available(3662220U, 100U));
  assert(boundary.usedMs() == 0U);

  // Normal wrap is safe; backward time fails closed.
  RfDutyBudget wrap;
  assert(wrap.reserve(UINT32_MAX - 1000U, 56U));
  wrap.finish(1000U);
  assert(wrap.available(2000U, 100U));
  assert(!wrap.available(1999U, 100U));
  assert(wrap.faulted());

  puts("ORUN-201 rolling budget, boundary, wrap, clock fault: PASS");
}


void checkPhysicalBudgetExhaustion() {
  Grants grants;
  Nor flash;
  ProductCustodyStore store(flash, 2);
  assert(store.begin());

  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(
      owner, grants, kGateway);

  uint8_t received[100]{};
  frame(received, sizeof(received));

  assert(ingress.receive(
      received, sizeof(received), true) ==
      GatewayCustodyAuthorizedIngress::Result::kStarted);

  for (unsigned n = 0;
       n < 150 && !ingress.batchReady(); ++n)
    ingress.service();

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  assert(ingress.takeAuthorizedBatchHandle(handle, scope));

  RadioManager radio;
  TestSequence sequences;

  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  uint8_t tx[100]{};
  frame(tx, sizeof(tx));
  put64(tx + 8, kGateway);

  const uint32_t cost = RfDutyBudget::costMs(100U);

  for (unsigned i = 0; i < 162U; ++i) {
    put64(tx + 20, 1000U + i);

    const uint32_t before = radio.rfAirtimeUsedMs();

    assert(radio.sendProductSecurePacket(tx, sizeof(tx)));

    assert(radio.rfAirtimeUsedMs() == before + cost);
    assert(send_calls == i + 1U);

    // No double charge while a physical TX is in progress.
    assert(!radio.sendProductSecurePacket(tx, sizeof(tx)));
    assert(radio.rfAirtimeUsedMs() == before + cost);

    test_now += 2220U;
    terminal(false);
    radio.update(false);
  }

  const uint32_t used = radio.rfAirtimeUsedMs();

  assert(used == 162U * cost);
  assert(!radio.gatewayAirtimeAvailable(56U));

  // Role changes must never create additional airtime.
  radio.setRole(NodeRole::kTracker);
  radio.update(false);
  radio.update(false);

  radio.enableSharedAirtimeBudget();

  assert(radio.rfAirtimeUsedMs() == used);

  radio.setRole(NodeRole::kBase);
  radio.update(false);
  radio.update(false);

  radio.enableGatewayBudget();

  assert(radio.rfAirtimeUsedMs() == used);

  using Result = RadioManager::BatchTx;

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now)
      == Result::kDeferred);

  assert(send_calls == 162U);
  assert(radio.rfAirtimeUsedMs() == used);

  printf(
      "ORUN-201 P0: used=%lu remaining=%lu "
      "batch_ack_cost=%lu physical_ack=0 PASS\n",
      (unsigned long)used,
      (unsigned long)(360000U - used),
      (unsigned long)RfDutyBudget::costMs(56U));

  // Old charges must eventually expire without reboot.
  test_now += 3663000U;
  radio.update(false);

  assert(radio.gatewayAirtimeAvailable(56U));
  assert(radio.rfAirtimeUsedMs() == 0U);

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now)
      == Result::kStarted);

  assert(send_calls == 163U);
  assert(radio.rfAirtimeUsedMs() ==
         RfDutyBudget::costMs(56U));

  // A physical TX timeout must not refund airtime.
  test_now += 2000U;
  terminal(true);
  radio.update(false);

  assert(radio.rfAirtimeUsedMs() ==
         RfDutyBudget::costMs(56U));

  puts("ORUN-201 physical TX, role, expiry, timeout: PASS");
}

void checkMixedRollingHours() {
  struct Interval {
    uint32_t start;
    uint32_t end;
  };

  std::vector<Interval> sent;

  RfDutyBudget budget;

  uint32_t now = 0U;
  unsigned rejected = 0U;
  unsigned attempts = 0U;

  const uint8_t sizes[] = {
      34U, 73U, 100U, 84U,
      56U, 88U, 255U, 64U
  };

  while (now < 10800000U) {
    const uint8_t size = sizes[attempts++ % 8U];

    if (!budget.reserve(now, size)) {
      ++rejected;
      now += 1000U;
      continue;
    }

    const uint32_t duration =
        (profileAirtimeUs(size) + 999U) / 1000U;

    sent.push_back({now, now + duration});

    now += duration;
    budget.finish(now);
    now += 17U;
  }

  uint32_t maximum = 0U;

  auto check = [&](uint32_t start) {
    const uint32_t end = start + 3600000U;
    uint32_t total = 0U;

    for (const auto& tx : sent) {
      const uint32_t lo =
          tx.start > start ? tx.start : start;
      const uint32_t hi =
          tx.end < end ? tx.end : end;

      if (hi > lo) total += hi - lo;
    }

    assert(total <= 360000U);
    if (total > maximum) maximum = total;
  };

  for (const auto& tx : sent) {
    check(tx.start);
    check(tx.end);

    if (tx.start >= 3600000U)
      check(tx.start - 3600000U);

    if (tx.end >= 3600000U)
      check(tx.end - 3600000U);
  }

  assert(rejected > 0U);
  assert(maximum > 330000U);

  printf(
      "ORUN-201 mixed 3h: tx=%lu rejected=%u "
      "max_rolling_airtime_ms=%lu PASS\n",
      (unsigned long)sent.size(),
      rejected,
      (unsigned long)maximum);
}


void checkAllPhysicalKinds() {
  RadioManager radio;
  TestSequence sequences;
  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  auto verify = [&](const char* name, uint8_t size,
                    uint32_t before, uint32_t calls) {
    assert(send_calls == calls + 1U);
    assert(last_tx_size == size);
    assert(radio.rfAirtimeUsedMs() ==
           before + RfDutyBudget::costMs(size));

    test_now += RfDutyBudget::costMs(size);
    terminal(false);
    radio.update(false);

    assert(!radio.isTransmitting());

    printf("ORUN-201 TX %s size=%u cost=%lu PASS\n",
           name, unsigned(size),
           (unsigned long)RfDutyBudget::costMs(size));
  };

  uint8_t position[tlp::kPositionPacketSize]{};
  makePosition(kGateway, 1U, position);

  uint32_t before = radio.rfAirtimeUsedMs();
  uint32_t calls = send_calls;

  // Rejected driver lock must cost nothing.
  fake_task = 2;
  radio_driver::acquire();
  fake_task = 1;

  assert(!radio.sendPositionPacket(position));
  assert(send_calls == calls);
  assert(radio.rfAirtimeUsedMs() == before);

  fake_task = 2;
  radio_driver::release();
  fake_task = 1;

  assert(radio.sendPositionPacket(position));
  verify("POSITION", sizeof(position), before, calls);

  tlp::HistorySecurePacket history{};
  history.security_context =
      tlp::kHistorySecurityContextDeviceD2a;
  history.app_family =
      tlp::kHistoryAppFamilyObservation;
  history.ciphertext_len =
      tlp::kHistoryObservationPlaintextSize;
  history.device_id = kGateway;
  history.key_epoch = 7U;
  history.security_counter = 1U;
  history.history_incarnation = 1U;

  uint8_t history_wire[
      tlp::kHistoryObservationPacketSize]{};

  assert(tlp::serializeHistorySecurePacket(
      history, history_wire, sizeof(history_wire)));

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  assert(radio.sendHistorySecurePacket(
      history_wire, sizeof(history_wire)));

  verify("HISTORY_SECURE", sizeof(history_wire),
         before, calls);

  for (uint8_t size : {100U, 84U}) {
    uint8_t wire[100]{};
    frame(wire, size);
    put64(wire + 8, kGateway);

    before = radio.rfAirtimeUsedMs();
    calls = send_calls;

    assert(radio.sendProductSecurePacket(wire, size));

    verify(size == 100U ? "PERIODIC" : "EVENT",
           size, before, calls);
  }

  // Maximum-size command. Shape-only RF test.
  uint8_t command[255]{};
  command[0] = lora_command::kVersion;
  command[1] = lora_command::kCommandType;
  command[3] = lora_command::kResourceAreas;
  command[33] = 2U;
  command[34] = 1U;
  command[36] = 26U;

  assert(lora_command::wellFormed(
      command, sizeof(command)));

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  assert(radio.sendLoraFrame(
      command, sizeof(command)));

  verify("COMMAND", sizeof(command), before, calls);

  radio.setRole(NodeRole::kTracker);
  radio.update(false);
  radio.update(false);

  uint8_t result[lora_command::kResultSize]{};
  result[0] = lora_command::kVersion;
  result[1] = lora_command::kResultType;
  put64(result + 4, kGateway);

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  assert(radio.sendLoraFrame(
      result, sizeof(result)));

  verify("RESULT", sizeof(result), before, calls);

  // Tracker RESULT did not overwrite the unread Gateway terminal.
  DownlinkTxResult command_terminal{};
  assert(radio.takeDownlinkTxResult(command_terminal));
  assert(command_terminal.kind == DownlinkTxResult::Kind::kCommand);
  assert(command_terminal.succeeded);

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  RadioBudgetHostProbe::sendTest(radio);

  verify("TEST", tlp::kTestPacketSize, before, calls);

  radio.setRole(NodeRole::kRelay);
  radio.update(false);
  radio.update(false);

  makePosition(0x2222U, 2U, position);

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  rxDone(position, sizeof(position), -80, 5);
  radio.update(false);

  test_now += relay_config::kMaximumDelayMs;
  radio.update(false);

  verify("RELAY", tlp::kRelayForwardPacketSize,
         before, calls);

  radio.setRole(NodeRole::kBase);
  radio.update(false);
  radio.update(false);

  Nor flash;
  Grants grants;
  ProductCustodyStore store(flash, 2U);
  assert(store.begin());

  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(
      owner, grants, kGateway);

  uint8_t object[100]{};
  frame(object, sizeof(object));

  // Retired protocol must not create an ACK or mutate flash.
  const int initial_writes = flash.writes;

  assert(ingress.receive(object, sizeof(object), false) ==
         GatewayCustodyAuthorizedIngress::Result::kDenied);

  assert(flash.writes == initial_writes);
  assert(!ingress.pending());
  assert(!owner.batchReady());

  puts("ORUN-305 RETIRED ACK admission=DENIED flash_writes=0 PASS");

  assert(ingress.receive(object, sizeof(object), true) ==
         GatewayCustodyAuthorizedIngress::Result::kStarted);

  for (unsigned i = 0U;
       i < 150U && !ingress.batchReady(); ++i)
    ingress.service();

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  assert(ingress.takeAuthorizedBatchHandle(handle, scope));

  before = radio.rfAirtimeUsedMs();
  calls = send_calls;

  fake_task = 2;
  radio_driver::acquire();
  fake_task = 1;

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now) ==
      RadioManager::BatchTx::kDeferred);

  fake_task = 2;
  radio_driver::release();
  fake_task = 1;

  assert(send_calls == calls);
  assert(radio.rfAirtimeUsedMs() == before);

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now) ==
      RadioManager::BatchTx::kStarted);

  assert(last_tx[1] == kq05_gateway_batch_ack::kType);

  verify("SINGLE_ACK_0x0A", 56U, before, calls);

  puts("ORUN-201 individual physical TX accounting: PASS");
}


void checkSleepBudget() {
  RadioManager radio;
  TestSequence sequences;
  beginAs(radio, sequences, NodeRole::kTracker);

  uint8_t object[100]{};
  frame(object, sizeof(object));
  put64(object + 8, kLocalDevice);

  const uint32_t cost = RfDutyBudget::costMs(100U);

  assert(radio.rfAirtimeUsedMs() == 0U);
  assert(radio.sendProductSecurePacket(
      object, sizeof(object)));

  test_now += cost;
  terminal(false);
  radio.update(false);

  assert(radio.rfAirtimeUsedMs() == cost);

  test_now += radio_config::kWindowedRxAfterTxMs + 1U;
  radio.update(false);
  radio.update(false);

  assert(radio.listenState() == RadioListenState::kAsleep);
  assert(radio.rfAirtimeUsedMs() == cost);

  assert(radio.sendProductSecurePacket(
      object, sizeof(object)));

  assert(radio.rfAirtimeUsedMs() == 2U * cost);

  test_now += cost;
  terminal(false);
  radio.update(false);

  assert(radio.rfAirtimeUsedMs() == 2U * cost);

  puts("ORUN-201 Tracker cold TX, sleep, wake, retained budget: PASS");
}

}


void checkOrun305CommandPhysicalAdmission() {
  RadioManager radio;
  TestSequence sequences;

  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  static RadioManager* active = nullptr;

  active = &radio;

  LoraCommandRelay relay;

  uint8_t command[255]{};

  command[0] = lora_command::kVersion;
  command[1] = lora_command::kCommandType;
  command[3] = lora_command::kResourceAreas;

  for (unsigned i = 0U; i < 8U; ++i)
    command[4U + i] =
        static_cast<uint8_t>(
            kTracker >> (56U - i * 8U));

  command[19] = 35U;
  command[33] = 2U;
  command[34] = 1U;
  command[36] = 26U;

  assert(lora_command::wellFormed(
      command, sizeof(command)));

  assert(relay.queue(command, sizeof(command)) ==
         LoraCommandRelay::Queued::kAdded);

  const auto budget = [](uint8_t size) {
    return active->gatewayAirtimeAvailable(size);
  };

  const auto start =
      [](const LoraCommandRelay::Frame& f) {
    return active->sendLoraFrame(
        f.bytes, f.size, &f.rx_floor_ms);
  };

  LoraCommandRelay::Frame out{};

  uint64_t dropped_target = 0U;
  uint32_t dropped_id = 0U;

  // Fail final driver admission in six distinct windows.
  for (unsigned i = 0U; i < 6U; ++i) {
    test_now = 1000U + i * 60000U;

    relay.heard(kTracker, test_now);

    test_now += LoraCommandRelay::kAckGraceMs;

    fake_task = 2;
    radio_driver::acquire();
    fake_task = 1;

    assert(!relay.trySend(
        test_now, out, budget, start));

    fake_task = 2;
    radio_driver::release();
    fake_task = 1;

    assert(out.attempt == 1U);

    assert(send_calls == 0U);
    assert(radio.rfAirtimeUsedMs() == 0U);

    assert(relay.count() == 1U);

    assert(!relay.takeDropped(
        dropped_target, dropped_id));
  }

  puts("ORUN-305 RADIO DENIALS physical_tx=0 attempts=0 budget=0 PASS");

  const uint32_t floor = 380000U;
  const uint32_t cutoff =
      radio_config::kWindowedRxAfterTxMs -
      (profileAirtimeUs(255U) + 999U) / 1000U -
      kAckTurnaroundGuardMs;

  relay.heard(kTracker, floor);
  test_now = floor + cutoff;

  assert(!relay.trySend(
      test_now, out, budget, start));

  assert(send_calls == 0U);
  assert(radio.rfAirtimeUsedMs() == 0U);

  // New window: preparation initially fits, but standby
  // consumes the final millisecond before admission.
  const uint32_t next_floor = 390000U;

  relay.heard(kTracker, next_floor);

  test_now = next_floor + cutoff - 1U;

  standby_cost_ms = 1U;

  assert(!relay.trySend(
      test_now, out, budget, start));

  standby_cost_ms = 0U;

  assert(send_calls == 0U);
  assert(radio.rfAirtimeUsedMs() == 0U);

  radio.update(false);

  assert(radio.canSend());

  puts("ORUN-305 COMMAND RX DEADLINE late=0 standby=0 attempts=0 PASS");

  // Next real driver admission is still attempt one.
  test_now = 400000U;

  relay.heard(kTracker, test_now);

  test_now += LoraCommandRelay::kAckGraceMs;

  assert(relay.trySend(
      test_now, out, budget, start));

  assert(out.attempt == 1U);
  assert(out.part == 0U);

  assert(send_calls == 1U);

  assert(radio.rfAirtimeUsedMs() ==
         RfDutyBudget::costMs(255U));

  assert(last_tx_size == 255U);

  assert(!memcmp(
      last_tx, command, sizeof(command)));

  const uint32_t air_us =
      profileAirtimeUs(255U);

  const uint32_t air_ms =
      (air_us + 999U) / 1000U;

  assert(air_ms == 5002U);

  assert(radio_config::kTxTimeoutMs >=
         air_ms + 500U);

  // Old timeout would abort this operation.
  test_now += 5000U;

  radio.update(false);

  assert(radio.isTransmitting());
  assert(radio.txTimeouts() == 0U);

  test_now += air_ms - 5000U;

  terminal(false);
  radio.update(false);

  assert(!radio.isTransmitting());
  assert(radio.txTimeouts() == 0U);

  printf("ORUN-305 MAX COMMAND bytes=255 "
         "airtime_us=%lu timeout_ms=%lu "
         "TX_DONE=PASS\n",
         (unsigned long)air_us,
         (unsigned long)radio_config::kTxTimeoutMs);

  DownlinkTxResult completed{};
  assert(radio.takeDownlinkTxResult(completed));
  assert(completed.kind == DownlinkTxResult::Kind::kCommand && completed.succeeded);
  relay.onTxResult(completed.target, completed.command_id, completed.part, completed.succeeded);

  // A genuinely missing TX terminal still times out.
  test_now += 60000U;

  relay.heard(kTracker, test_now);

  test_now += LoraCommandRelay::kAckGraceMs;

  assert(relay.trySend(
      test_now, out, budget, start));

  assert(out.attempt == 2U);

  test_now += radio_config::kTxTimeoutMs - 1U;

  radio.update(false);

  assert(radio.isTransmitting());

  ++test_now;

  radio.update(false);

  assert(!radio.isTransmitting());
  assert(radio.txTimeouts() == 1U);

  puts("ORUN-305 MISSING TX_DONE bounded_timeout=PASS");

  active = nullptr;
}



void checkOrun305SingleAckBudgetAndRevocation() {
  RadioManager radio;
  TestSequence sequences;

  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  Nor flash;
  Grants grants;
  ProductCustodyStore store(flash, 2U);

  assert(store.begin());

  GatewayLocalCustodyOwner owner(store);

  GatewayCustodyAuthorizedIngress ingress(
      owner, grants, kGateway);

  uint8_t object[100]{};

  frame(object, sizeof(object));

  assert(ingress.receive(object, sizeof(object), true) ==
         GatewayCustodyAuthorizedIngress::Result::kStarted);

  for (unsigned i = 0U;
       i < 150U && !ingress.batchReady(); ++i)
    ingress.service();

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  assert(ingress.takeAuthorizedBatchHandle(handle, scope));

  uint8_t tx[100]{};

  frame(tx, sizeof(tx));

  put64(tx + 8, kGateway);

  for (unsigned i = 0U; i < 162U; ++i) {
    put64(tx + 20, 1000U + i);

    assert(radio.sendProductSecurePacket(tx, sizeof(tx)));

    test_now += 2220U;

    terminal(false);
    radio.update(false);
  }

  const uint32_t used = radio.rfAirtimeUsedMs();

  assert(used == 359154U);

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now) ==
      RadioManager::BatchTx::kDeferred);

  assert(send_calls == 162U);
  assert(radio.rfAirtimeUsedMs() == used);

  uint8_t exact[100]{};

  size_t length = 0U;

  assert(store.readConfirmedDurable(
      handle, exact, length) ==
      ProductCustodyStore::Lookup::kFound);

  assert(length == sizeof(object));
  assert(memcmp(exact, object, length) == 0);

  puts("ORUN-305 SINGLE ACK budget_denied=1 exact_retained=1 PASS");

  test_now += 3663000U;

  radio.update(false);

  assert(radio.gatewayAirtimeAvailable(56U));

  grants.generation = 2U;

  assert(radio.sendAuthorizedBatchAck(
      ingress, &handle, 1U, scope, test_now) ==
      RadioManager::BatchTx::kUnverifiable);

  assert(send_calls == 162U);
  assert(radio.rfAirtimeUsedMs() == 0U);

  puts("ORUN-305 SINGLE ACK revoked_tx=0 PASS");
}


void checkOrun305RelayAdmission() {
  RadioManager radio;
  TestSequence sequences;

  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  uint8_t tx[100]{};
  frame(tx, sizeof(tx));
  put64(tx + 8, kGateway);

  for (unsigned i = 0U; i < 162U; ++i) {
    put64(tx + 20, 1000U + i);
    assert(radio.sendProductSecurePacket(tx, sizeof(tx)));

    test_now += 2220U;
    terminal(false);
    radio.update(false);
  }

  const uint32_t used = radio.rfAirtimeUsedMs();

  assert(!radio.gatewayAirtimeAvailable(
      tlp::kRelayForwardPacketSize));

  radio.setRole(NodeRole::kRelay);
  radio.update(false);
  radio.update(false);

  uint8_t position[tlp::kPositionPacketSize]{};
  makePosition(0x2222U, 42U, position);

  rxDone(position, sizeof(position), -94, 5);
  radio.update(false);

  assert(radio.relayDiagnostics().queued == 1U);

  const uint32_t calls = send_calls;

  test_now += relay_config::kMaximumDelayMs;

  for (unsigned n = 0U; n < 6U; ++n) {
    radio.update(false);
    test_now += 100U;

    assert(send_calls == calls);
    assert(radio.rfAirtimeUsedMs() == used);
    assert(radio.relayDiagnostics().forwards_attempted == 0U);
    assert(radio.relayDiagnostics().forward_failures == 0U);
  }

  // Let rolling-hour budget recover, then deny the
  // radio driver gate without consuming queued work.
  test_now += 3663000U;

  fake_task = 2;
  radio_driver::acquire();
  fake_task = 1;

  radio.update(false);

  assert(send_calls == calls);
  assert(radio.relayDiagnostics().forwards_attempted == 0U);

  fake_task = 2;
  radio_driver::release();
  fake_task = 1;

  radio.update(false);

  assert(send_calls == calls + 1U);
  assert(last_tx_size == tlp::kRelayForwardPacketSize);
  assert(radio.relayDiagnostics().forwards_attempted == 1U);

  assert(radio.rfAirtimeUsedMs() ==
         RfDutyBudget::costMs(tlp::kRelayForwardPacketSize));

  tlp::RelayForwardPacket envelope{};
  tlp::PositionPacket decoded{};

  assert(tlp::deserializeRelayForwardPacket(
      last_tx, last_tx_size, &envelope, &decoded) ==
      tlp::RelayDecodeStatus::kOk);

  assert(memcmp(envelope.original_packet, position,
                sizeof(position)) == 0);

  assert(envelope.ingress_rssi_dbm == -94);
  assert(envelope.ingress_snr_db == 5);

  test_now += 1300U;
  terminal(false);
  radio.update(false);

  assert(radio.relayDiagnostics().forwards_completed == 1U);
  assert(radio.relayDiagnostics().forward_failures == 0U);

  test_now += 10000U;
  radio.update(false);

  assert(send_calls == calls + 1U);

  puts("ORUN-305 RELAY actual_radio budget_denied=6 gate_denied=1 exact_tx=1 PASS");
}


static void checkCommandTerminalHandoff() {
  RadioManager radio;
  TestSequence sequences;

  radio.setDeviceIdentity(
      DeviceIdentity::fromLegacyUint64(kGateway));

  beginAs(radio, sequences, NodeRole::kBase);

  namespace lc = orun_tlp::lora_command;

  uint8_t cmd[54]{};
  cmd[0] = lc::kVersion;
  cmd[1] = lc::kCommandType;
  cmd[3] = lc::kResourceSettings;

  for (unsigned i = 0; i < 8U; ++i)
    cmd[4U + i] = uint8_t(kTracker >> (56U - 8U*i));

  cmd[19] = 17U;
  cmd[32] = 1U;
  cmd[33] = 1U;

  assert(lc::wellFormed(cmd, sizeof(cmd)));

  test_now = 1000U;
  uint32_t floor = test_now;

  assert(radio.sendLoraFrame(cmd, sizeof(cmd), &floor));

  DownlinkTxResult outcome{};
  assert(!radio.takeDownlinkTxResult(outcome));

  test_now += 2000U;
  terminal(false);
  radio.update(false);

  assert(radio.takeDownlinkTxResult(outcome));
  assert(outcome.kind == DownlinkTxResult::Kind::kCommand);
  assert(outcome.target == kTracker);
  assert(outcome.command_id == 17U);
  assert(outcome.part == 0U);
  assert(outcome.rx_floor_ms == floor);
  assert(outcome.succeeded);
  assert(!radio.takeDownlinkTxResult(outcome));

  test_now += 1000U;
  floor = test_now;
  cmd[19] = 18U;

  assert(radio.sendLoraFrame(cmd, sizeof(cmd), &floor));

  const uint32_t charged = radio.rfAirtimeUsedMs();

  test_now += 6000U;
  radio.update(false);

  assert(radio.takeDownlinkTxResult(outcome));
  assert(outcome.kind == DownlinkTxResult::Kind::kCommand);
  assert(outcome.command_id == 18U);
  assert(outcome.target == kTracker);
  assert(outcome.rx_floor_ms == floor);
  assert(!outcome.succeeded);
  assert(!radio.takeDownlinkTxResult(outcome));
  assert(radio.rfAirtimeUsedMs() == charged);

  // Short first command: only the 7 s limit, not airtime, binds.
  test_now += 10000U;
  floor = test_now;
  test_now = floor + 6999U;
  standby_cost_ms = 2U;
  const auto before_attempts = radio.txAttempts();
  const auto before_calls = send_calls;
  const auto before_budget = radio.rfAirtimeUsedMs();
  assert(ackFitsWindow(test_now + 2U, floor, sizeof(cmd)));
  assert(!radio.sendLoraFrame(cmd, sizeof(cmd), &floor));
  assert(radio.txAttempts() == before_attempts);
  assert(send_calls == before_calls && radio.rfAirtimeUsedMs() == before_budget);
  standby_cost_ms = 0U;
  radio.update(false);
  assert(radio.canSend());
  assert(!radio.sendLoraFrame(cmd, sizeof(cmd), &floor));
  radio.update(false);

  floor = test_now + 10000U;
  test_now = floor + 6999U;
  assert(radio.sendLoraFrame(cmd, sizeof(cmd), &floor));
  assert(radio.txAttempts() == before_attempts + 1U);
  test_now += RfDutyBudget::costMs(sizeof(cmd));
  terminal(false);
  radio.update(false);
  assert(radio.takeDownlinkTxResult(outcome) && outcome.succeeded);

  // A later multipart frame uses the extended RX window, not the
  // first-part 7 s limit. Valid shape: 210 bytes split into 202+8.
  uint8_t part[lc::kAreaHeaderSize + 8U + lc::kTagSize]{};
  memcpy(part, cmd, 32U);
  part[3] = lc::kResourceAreas;
  part[32] = 1U;
  part[33] = 2U;
  part[34] = 1U;
  part[36] = 26U;
  assert(lc::wellFormed(part, sizeof(part)));
  floor = test_now + 10000U;
  test_now = floor + 7500U;
  assert(radio.sendLoraFrame(part, sizeof(part), &floor));
  test_now += RfDutyBudget::costMs(sizeof(part));
  terminal(false);
  radio.update(false);
  assert(radio.takeDownlinkTxResult(outcome) && outcome.part == 1U && outcome.succeeded);
  puts("ORUN-305 P2 final first-command deadline / continuation PASS");
  puts("ORUN-305 REAL RADIO COMMAND TERMINAL PASS");
}

int main() {
  checkCommandTerminalHandoff();
  checkOrun305RelayAdmission();
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (uint8_t count = 1; count <= 5; ++count) checkSignedWindow(count);
  checkSharedBudget();
  checkPhysicalBudgetExhaustion();
  checkMixedRollingHours();
  checkAllPhysicalKinds();
  checkOrun305CommandPhysicalAdmission();
  checkOrun305SingleAckBudgetAndRevocation();
  checkSleepBudget();
  assert(radio_owner_regressions() == 0);
  puts("Real signer + radio deadline, wrap, busy, cold budget, 1..5, TX_DONE: PASS");
}
