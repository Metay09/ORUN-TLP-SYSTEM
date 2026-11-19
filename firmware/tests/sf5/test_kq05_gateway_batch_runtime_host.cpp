// KQ05: real Gateway OCQ2 + 1..5 scoped batch HMAC + LIVE ingress.
// Only the RadioManager transport is mocked. No physical RF/Flash.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kq05_gateway_batch_runtime.h"

#define main kq05_durable_gateway_fixture_main
#include "test_kq05_gateway_durable_batch_owner_host.cpp"
#undef main

namespace {
bool g_rx_pending=false;
bool g_tx_allowed=true;
bool g_tx_admitted=true;
unsigned g_tx_sends=0U;
uint64_t g_device_id=kGateway;
orun_tlp::ProductSecureRxFrame g_rx{};
uint8_t g_tx_wire[ack::kMaxSize]{};
size_t g_tx_length=0U;

void resetRadio() {
  g_rx_pending=false;
  g_tx_allowed=true;
  g_tx_admitted=true;
  g_tx_sends=0U;
  g_device_id=kGateway;
  g_rx={};
  memset(g_tx_wire,0,sizeof(g_tx_wire));
  g_tx_length=0U;
}
void push(uint32_t seq,bool custody=true,uint64_t tracker=kTracker) {
  assert(!g_rx_pending);
  g_rx={};
  g_rx.size=100U;
  frame(g_rx.payload,g_rx.size,seq,tracker);
  if (!custody) g_rx.payload[4U]=1U; // relay allowed, NO custody intent
  g_rx.rssi_dbm=-67;
  g_rx.snr_db=7;
  g_rx_pending=true;
}
} // namespace

namespace orun_tlp {
bool RadioManager::canSend() const { return g_tx_allowed; }
uint64_t RadioManager::deviceId() const { return g_device_id; }
bool RadioManager::takeProductSecureFrame(ProductSecureRxFrame* out) {
  if (!out || !g_rx_pending) return false;
  *out=g_rx;
  g_rx={};
  g_rx_pending=false;
  return true;
}
bool RadioManager::sendAuthorizedKq05GatewayBatchAck(
    Kq05GatewayBatchCollector& owner) {
  if (!canSend() || deviceId()!=owner.localGatewayId()) return false;
  uint8_t candidate[ack::kMaxSize]{};
  size_t n=0U;
  if (owner.prepare(candidate,n)!=
      Kq05GatewayBatchCollector::Prepare::kReady) return false;
  ack::Header h{};
  if (!ack::decode(candidate,n,h) || h.gateway_device_id!=deviceId())
    return false;
  if (!g_tx_admitted) return false; // failed driver gate/Radio admission
  memcpy(g_tx_wire,candidate,n);
  g_tx_length=n;
  ++g_tx_sends;
  return owner.markTxAdmitted();
}
} // namespace orun_tlp

namespace {
using Runtime=orun_tlp::Kq05GatewayBatchRuntime;
using Collector=orun_tlp::Kq05GatewayBatchCollector;
using Live=orun_tlp::GatewayProductLiveIngress;

Runtime::Event pushAndCommit(Runtime& runtime,uint32_t seq,
                             uint32_t when,uint64_t tracker=kTracker) {
  push(seq,true,tracker);
  assert(runtime.update(true,when)==Runtime::Event::kCustodyStarted);
  auto result=Runtime::Event::kNone;
  for(unsigned i=0U;i<50U;++i) {
    result=runtime.update(true,when+1U+i);
    if(result!=Runtime::Event::kNone) break;
  }
  assert(result==Runtime::Event::kCustodyDurable);
  return result;
}

void verifyAck(const Fixture& f,uint8_t count,size_t len) {
  assert(g_tx_sends>0U && g_tx_length==len);
  ack::Header h{};
  assert(ack::decode(g_tx_wire,g_tx_length,h));
  assert(h.count==count && h.tracker_device_id==kTracker);
  uint8_t key[32]{};
  Grants::fixedKey(key);
  // Actual durable exact stored bytes; order is canonical in signer.
  uint8_t objects[5][100]{};
  Kq05BatchExactView exact[5]{};
  for(uint8_t i=0U;i<count;++i) {
    const size_t n=(i+1U)&1U?100U:84U;
    frame(objects[i],n,i+1U);
    exact[i].bytes=objects[i];
    exact[i].size=n;
  }
  assert(orun_tlp::Kq05BatchCryptoHostPeer::verify(
      g_tx_wire,g_tx_length,f.grants.scope(),key,exact,count));
}

void testFiveImmediateAndLostAckDuplicate() {
  resetRadio();
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  Live live(f.store,f.grants,kGateway);
  orun_tlp::RadioManager radio;
  Runtime rt(radio,c,live,120000U);
  for(uint32_t n=1U;n<=5U;++n) {
    pushAndCommit(rt,n,n*1000U);
    assert(c.queued()==n);
    assert(c.flushRequested()==(n==5U));
  }
  assert(rt.partialTimerArmed() && c.flushRequested());
  const size_t writes=f.flash.writes;
  assert(rt.update(true,5007U)==Runtime::Event::kAckTxStarted);
  verifyAck(f,5U,88U);
  assert(f.flash.writes==writes);
  assert(c.queued()==0U && !rt.partialTimerArmed());

  // Lost downlink does not undo durable OCQ2. Duplicate custody byte
  // re-enrolls without another Flash program and can be re-ACKed.
  push(1U);
  assert(rt.update(true,6000U)==Runtime::Event::kDuplicate);
  assert(f.flash.writes==writes && c.queued()==1U);
  assert(rt.update(true,6000U+120000U-1U)==Runtime::Event::kNone);
  assert(rt.update(true,6000U+120000U)==Runtime::Event::kAckTxStarted);
  assert(g_tx_length==56U && g_tx_sends==2U);
  assert(f.flash.writes==writes);
}

void testPartialOneThroughFourAndNoRollingDeadline() {
  for(uint32_t n=1U;n<=4U;++n) {
    resetRadio();
    Fixture f;
    Collector c(f.store,f.grants,kGateway);
    Live live(f.store,f.grants,kGateway);
    orun_tlp::RadioManager radio;
    Runtime rt(radio,c,live,30000U);
    for(uint32_t seq=1U;seq<=n;++seq)
      pushAndCommit(rt,seq,100U+seq*1000U);
    assert(!c.flushRequested() && c.queued()==n);
    const uint32_t first=rt.firstDurableMs();
    assert(first==1101U); // durable readback, not first raw RX
    assert(rt.update(true,first+29999U)==Runtime::Event::kNone);
    assert(rt.update(true,first+30000U)==Runtime::Event::kAckTxStarted);
    verifyAck(f,static_cast<uint8_t>(n),48U+8U*n);
    assert(c.queued()==0U);
  }
}

void testFullGroupCannotBlockIndependentLive() {
  resetRadio();
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  Live live(f.store,f.grants,kGateway);
  orun_tlp::RadioManager radio;
  Runtime rt(radio,c,live,120000U);
  for(uint32_t n=1U;n<=5U;++n)
    pushAndCommit(rt,n,100U*n);
  assert(c.queued()==5U && c.flushRequested());
  g_tx_allowed=false; // Lost/blocked downlink.
  assert(rt.update(true,1000U)==Runtime::Event::kNone);
  assert(c.queued()==5U);
  push(20U,false);
  assert(rt.update(true,1001U)==Runtime::Event::kLiveStarted);
  assert(live.pending());
  auto result=Runtime::Event::kNone;
  for(unsigned i=0U;i<50U;++i) {
    result=rt.update(true,1002U+i);
    if(result!=Runtime::Event::kNone) break;
  }
  assert(result==Runtime::Event::kLiveDurable);
  assert(!live.pending());
  assert(c.queued()==5U && g_tx_sends==0U);
  g_tx_admitted=false;
  g_tx_allowed=true;
  assert(rt.update(true,2000U)==Runtime::Event::kNone);
  assert(c.queued()==5U && g_tx_sends==0U);
  g_tx_admitted=true;
  assert(rt.update(true,2001U)==Runtime::Event::kAckTxStarted);
  assert(g_tx_sends==1U && c.queued()==0U);
}

void testRevokeDisableForeignTrackerAndWraparound() {
  resetRadio();
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  Live live(f.store,f.grants,kGateway);
  orun_tlp::RadioManager radio;
  Runtime rt(radio,c,live,30000U);
  pushAndCommit(rt,1U,UINT32_MAX-1000U);
  assert(rt.firstDurableMs()==UINT32_MAX-999U);
  assert(rt.update(false,29001U)==Runtime::Event::kNone);
  assert(c.flushRequested()); // wrap safe; DISABLED never emits
  assert(c.queued()==1U && g_tx_sends==0U);
  g_device_id=kGateway+1U;
  assert(rt.update(true,29002U)==Runtime::Event::kNone);
  assert(g_tx_sends==0U);
  g_device_id=kGateway;
  f.grants.active=false;
  assert(rt.update(true,29003U)==Runtime::Event::kNone);
  assert(c.queued()==1U && g_tx_sends==0U);
  f.grants.active=true;
  assert(rt.update(true,29004U)==Runtime::Event::kAckTxStarted);
  assert(c.queued()==0U && g_tx_sends==1U);

  // Different Tracker cannot be signed under the existing group.
  resetRadio();
  Fixture f2;
  Collector c2(f2.store,f2.grants,kGateway);
  Live live2(f2.store,f2.grants,kGateway);
  Runtime rt2(radio,c2,live2,120000U);
  pushAndCommit(rt2,1U,100U);
  push(2U,true,kTracker+1U);
  assert(rt2.update(true,200U)==Runtime::Event::kCustodyDeferred);
  assert(c2.flushRequested() && c2.queued()==1U);
  assert(rt2.update(true,201U)==Runtime::Event::kAckTxStarted);
  assert(g_tx_sends==1U && g_tx_length==56U);
}
} // namespace

int main() {
  kq05_durable_gateway_fixture_main();
  testFiveImmediateAndLostAckDuplicate();
  testPartialOneThroughFourAndNoRollingDeadline();
  testFullGroupCannotBlockIndependentLive();
  testRevokeDisableForeignTrackerAndWraparound();
  puts("KQ05 Gateway 1..5 OCQ2 timed HMAC and blocked-TX LIVE first: PASS (HOST ONLY)");
  puts("NO RF OVER AIR, NO BOARD FLASH, NO LEGACY ERASE, NO DEVICE UPLOAD");
  return 0;
}
