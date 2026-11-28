#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main old_driver
#include "firmware/tests/r2/test_r2.cpp"
#undef main
#define main old_ingress
#include "firmware/tests/sf5/test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main
#define main old_commands
#include "firmware/tests/m7/test_lora_command_relay.cpp"
#undef main
#pragma GCC diagnostic pop
#include "gateway_product_custody_runtime.h"
namespace orun_tlp {
struct RadioBudgetHostProbe {
  static void history(RadioManager& r) {
    const uint32_t charged=RfDutyBudget::costMs(255);
    for(unsigned i=0;i<71;++i) {
      assert(r.rf_budget_.reserve(i*charged,255));
      r.rf_budget_.finish((i+1)*charged);
    }
  }
};
}
void run(bool commandPending) {
  Grants grants; Nor nor; ProductCustodyStore store(nor,2); assert(store.begin());
  GatewayLocalCustodyOwner owner(store); GatewayCustodyAuthorizedIngress ingress(owner,grants,kGateway);
  RadioManager radio; TestSequence seq; radio.setDeviceIdentity(DeviceIdentity::fromLegacyUint64(kGateway));
  beginAs(radio,seq,NodeRole::kBase); radio.enableGatewayBudget();
  RadioBudgetHostProbe::history(radio);
  GatewayProductCustodyRuntime runtime(radio,ingress);
  LoraCommandRelay relay; LoraCommandRelay::Frame f{};
  auto parts=areaParts(kB,18,8,64);
  if(commandPending) for(const auto& p:parts) relay.queue(p.data(),p.size());
  const uint32_t t=360000;
  auto service=[&]() {
    uint64_t id=0;uint32_t heard=0;
    if(radio.takeHeardSender(id,heard)) relay.heard(id,heard);
    static RadioManager* port; port=&radio;
    if(radio.canSend()) relay.trySend(test_now,f,[](uint8_t n){return port->gatewayAirtimeAvailable(n);},[](const LoraCommandRelay::Frame& x){return port->sendLoraFrame(x.bytes,x.size,&x.rx_floor_ms);});

    if(!radio.isTransmitting()) runtime.update(true);
  };
  auto offer=[&](uint8_t* frameBytes) {
    radio.update(false); rxDone(frameBytes,100,-80,4); radio.update(false); service();
  };
  test_now=t;
  uint8_t a[100];frame(a,sizeof(a));offer(a);
  for(unsigned i=0;i<150;++i) service();
  assert(runtime.reservedPages() && !send_calls);
  assert(radio.gatewayAirtimeAvailable(56,2000000) && !radio.gatewayAirtimeAvailable(255));
  test_now=t+2000;
  uint8_t b[100];frame(b,sizeof(b),false);put64(b+8,kB);offer(b);
  for(unsigned dt=2000;dt<=9050 && !send_calls;dt+=50){
    test_now=t+dt; radio.update(false);service();
  }
  assert(send_calls == 1U);
  assert(runtime.reservedPages() == 0U);
  assert(relay.count() == (commandPending ? 1U : 0U));

  printf("other_peer_command_pending=%u first_peer_ack_starts=%u ack_still_pending=%u airtime_used=%u\n", commandPending,send_calls,!!runtime.reservedPages(),radio.rfAirtimeUsedMs());
}
int main(){run(false);run(true);}
