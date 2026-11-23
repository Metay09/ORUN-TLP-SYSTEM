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
  assert(sendBatch(test_now) == Result::kDeferred && !send_calls); // cold budget
  const uint8_t size = 48U + count * 8U;
  const uint32_t cutoff = 10000U - kAckTurnaroundGuardMs -
      (profileAirtimeUs(size) + 999U) / 1000U;
  const uint32_t heard = UINT32_MAX - 5000U; // deadline crosses clock wrap
  test_now = heard;
  assert(radio.gatewayAirtimeAvailable(size));
  test_now += cutoff - 2U;
  grants.delay_ms = 1; // both actual grant reads consume 2 ms during preparation
  assert(sendBatch(heard) == Result::kDeferred);
  assert(!send_calls && !radio.isTransmitting());
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
  kq05_gateway_batch_ack::Header decoded{};
  assert(kq05_gateway_batch_ack::decode(last_tx, last_tx_size, decoded));
  assert(decoded.count == count && decoded.tracker_device_id == kTracker);
  test_now += (profileAirtimeUs(size) + 999U) / 1000U;
  terminal(false);
  radio.update(false);
  assert(radio.eventDiagnostics().batch_ack_max_done_age_ms < 10000U);
}

void checkSharedBudget() {
  GatewayAirtimeBudget bucket;
  uint32_t hourly_us = 0;
  unsigned denied = 0;
  // Keep it saturated with mixed wire sizes for two hours, including a reboot.
  for (unsigned t = 0; t < 7200000U; t += 100U) {
    if (t == 3600000U) {
      assert(hourly_us <= 360000000U);
      hourly_us = 0;
      bucket = GatewayAirtimeBudget{};
    }
    const uint8_t size = t % 300U == 0 ? 255U : (t % 300U == 100U ? 56U : 88U);
    if (!bucket.available(t % 3600000U, size)) { ++denied; continue; }
    bucket.charge(size);
    hourly_us += profileAirtimeUs(size);
  }
  assert(denied && hourly_us > 330000000U && hourly_us <= 360000000U);
}
}

int main() {
  for (uint8_t count = 1; count <= 5; ++count) checkSignedWindow(count);
  checkSharedBudget();
  puts("Real signer + radio deadline, wrap, busy, cold budget, 1..5, TX_DONE: PASS");
}
