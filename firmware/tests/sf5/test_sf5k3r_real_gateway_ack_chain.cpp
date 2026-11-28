// Real 0x0A HMAC + OCQ2 readback + restart.
#define main sf5k3s_previous_main
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

void acceptedAfterRestart(size_t size, bool async) {
  Nor flash;
  flash.async = async;
  Grants grant;

  uint8_t object[100]{};
  frame(object, size);

  using Result = GatewayCustodyAuthorizedIngress::Result;

  {
    ProductCustodyStore store(flash, 2U);
    assert(store.begin());

    GatewayLocalCustodyOwner owner(store);
    GatewayCustodyAuthorizedIngress ingress(
        owner, grant, kGateway);

    assert(ingress.receive(object, size, true) ==
           Result::kStarted);

    ProductCustodyStore::Handle handle{};
    GatewayCustodyAckScope scope{};

    assert(!ingress.takeAuthorizedBatchHandle(handle, scope));

    for (unsigned i = 0; i < 100U &&
         !ingress.batchReady(); ++i) {
      ingress.service();
    }

    assert(ingress.takeAuthorizedBatchHandle(handle, scope));

    uint8_t wire[kq05_gateway_batch_ack::kMaxSize]{};
    size_t length = 0U;

    assert(ingress.prepareAuthorizedBatchAck(
        &handle, 1U, scope, wire, length));

    uint8_t ack[56]{};
    assert(length == sizeof(ack));

    memcpy(ack, wire, length);
    checkExpected(ack, size);
  }

  const int writes = flash.writes;
  const auto snapshot = flash.data;

  ProductCustodyStore recovered(flash, 2U);
  assert(recovered.begin());

  GatewayLocalCustodyOwner owner(recovered);
  GatewayCustodyAuthorizedIngress ingress(
      owner, grant, kGateway);

  assert(ingress.receive(object, size, true) ==
         Result::kAckReady);

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  assert(ingress.takeAuthorizedBatchHandle(handle, scope));

  uint8_t wire[kq05_gateway_batch_ack::kMaxSize]{};
  size_t length = 0U;

  assert(ingress.prepareAuthorizedBatchAck(
      &handle, 1U, scope, wire, length));

  uint8_t ack[56]{};

  assert(length == sizeof(ack));
  memcpy(ack, wire, length);
  checkExpected(ack, size);

  assert(flash.writes == writes);
  assert(flash.data == snapshot);
}

int main() {
  accepted(100U, false);
  accepted(84U, false);
  accepted(100U, true);
  accepted(84U, true);

  acceptedAfterRestart(100U, false);
  acceptedAfterRestart(84U, false);
  acceptedAfterRestart(100U, true);
  acceptedAfterRestart(84U, true);

  failedFlashNoAck();

  puts("ORUN-305 EXACT DURABLE HMAC AND REBOOT REACK PASS");

  return 0;
}
