// Actual 0x0A durable owner, host NOR only.
#define main sf5k3s_previous_main
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

void corruptionBeforeFinalReadback() {
  Nor nor;
  ProductCustodyStore store(nor, 2U);
  assert(store.begin());

  GatewayLocalCustodyOwner owner(store);
  Grants grant;
  GatewayCustodyAuthorizedIngress ingress(
      owner, grant, kGateway);

  uint8_t packet[100]{};
  frame(packet, sizeof(packet));

  assert(ingress.receive(packet, sizeof(packet), true) ==
         GatewayCustodyAuthorizedIngress::Result::kStarted);

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  for (unsigned i = 0; i < 5U; ++i) {
    assert(!ingress.takeAuthorizedBatchHandle(handle, scope));
    ingress.service();
  }

  assert(nor.writes == 5);
  assert(owner.waitingFlash());
  assert(!ingress.batchReady());

  nor.data[fmt::kPageHeaderSize + 10U] ^= 0x01U;

  ingress.service();

  assert(!ingress.batchReady());
  assert(!ingress.takeAuthorizedBatchHandle(handle, scope));
  assert(store.faulted());

  puts("ORUN-305 CORRUPTED READBACK ACK=0 PASS");
}

int main() {
  accepted(100U, false);
  accepted(100U, true);
  failedFlashNoAck();
  corruptionBeforeFinalReadback();

  puts("ORUN-305 DURABLE OWNER ADMISSION AND READBACK PASS");
  return 0;
}
