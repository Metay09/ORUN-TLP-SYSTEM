// KQ05 Gateway handle collector uses ACTUAL OCQ2 and scope+full-SHA MAC.
// Simulated NOR only; no RF, physical flash, history erase or device upload.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_gateway_batch_collector.h"
#define main kq05_durable_parent_main
#include "test_kq05_gateway_durable_batch_owner_host.cpp"
#undef main

namespace {
using Collector = orun_tlp::Kq05GatewayBatchCollector;
namespace ack=orun_tlp::kq05_gateway_batch_ack;
using BatchPeer=orun_tlp::Kq05BatchCryptoHostPeer;

void submitFrame(Collector& c, uint32_t seq, uint64_t tracker=kTracker) {
  uint8_t bytes[100]{};
  const size_t size=(seq & 1U)?100U:84U;
  frame(bytes,size,seq,tracker);
  assert(c.receive(bytes,size)==Collector::Receive::kStartedFlash);
  assert(c.pendingFlash());
  for (uint8_t turn=0U;turn<32U;++turn) {
    const auto result=c.service();
    if (result==Collector::Service::kQueued) return;
    assert(result==Collector::Service::kWaitingFlash);
  }
  assert(false && "Gateway OCQ2 commitment did not complete");
}

void testFiveFullAndExplicitPartialGroup() {
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  assert(c.queued()==0U);
  uint8_t out[ack::kMaxSize]{};
  memset(out,0xA5,sizeof(out));
  size_t written=456U;
  assert(c.prepare(out,written)==Collector::Prepare::kNotReady);
  assert(written==456U);
  assert(!c.requestFlush() && !c.markTxAdmitted());
  for (uint32_t seq=1U;seq<=5U;++seq) {
    submitFrame(c,seq);
    assert(c.queued()==seq);
    if (seq<5U) assert(!c.flushRequested());
  }
  assert(c.flushRequested() && c.queued()==5U);
  const size_t before=f.flash.writes;
  assert(c.prepare(out,written)==Collector::Prepare::kReady);
  assert(written==88U && f.flash.writes==before);
  ack::Header header{};
  assert(ack::decode(out,written,header));
  assert(header.count==5U);
  // Independent full group from same committed objects: identity and HMAC.
  uint8_t key[32]{};
  Grants::fixedKey(key);
  uint8_t exact[5][100]{};
  Kq05BatchExactView views[5]{};
  for (uint8_t i=0U;i<5U;++i) {
    const size_t size=(i+1U)&1U?100U:84U;
    frame(exact[i],size,i+1U);
    views[i].bytes=exact[i];
    views[i].size=size;
  }
  assert(BatchPeer::verify(out,written,f.grants.scope(),key,views,5U));

  // Failed/unavailable RF admission must NOT clear the batch.
  uint8_t retry[ack::kMaxSize]{};
  size_t retry_len=0U;
  assert(c.prepare(retry,retry_len)==Collector::Prepare::kReady);
  assert(retry_len==written && memcmp(retry,out,written)==0);
  assert(c.queued()==5U && f.flash.writes==before);
  assert(c.markTxAdmitted());
  assert(c.queued()==0U && !c.flushRequested());
  assert(!c.markTxAdmitted());

  // A lost downlink is repaired by re-ACKing *durable duplicate* exact
  // bytes, without writing new OCQ2 records.
  for (uint32_t seq=1U;seq<=2U;++seq) {
    uint8_t bytes[100]{};
    const size_t size=(seq&1U)?100U:84U;
    frame(bytes,size,seq);
    assert(c.receive(bytes,size)==Collector::Receive::kReenrolled);
  }
  assert(c.queued()==2U);
  assert(f.flash.writes==before);
  assert(c.requestFlush());
  size_t partial=0U;
  assert(c.prepare(out,partial)==Collector::Prepare::kReady);
  assert(partial==64U && f.flash.writes==before);
  views[0].bytes=exact[0];views[0].size=100U;
  views[1].bytes=exact[1];views[1].size=84U;
  assert(BatchPeer::verify(out,partial,f.grants.scope(),key,views,2U));
  assert(c.markTxAdmitted() && c.queued()==0U);
}

void testNoMixedTrackerOrInvalidCustody() {
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  submitFrame(c,1U);
  const size_t before=f.flash.writes;
  uint8_t foreign[100]{};
  frame(foreign,100U,2U,kTracker+1U);
  assert(c.receive(foreign,sizeof(foreign))==Collector::Receive::kBusy);
  assert(f.flash.writes==before && c.queued()==1U);
  assert(c.requestFlush());
  uint8_t blocked[100]{};
  frame(blocked,100U,3U);
  assert(c.receive(blocked,sizeof(blocked))==Collector::Receive::kBusy);
  uint8_t out[ack::kMaxSize]{};
  size_t n=0U;
  assert(c.prepare(out,n)==Collector::Prepare::kReady && n==56U);
  assert(c.markTxAdmitted());
  // Wrong Tracker/invalid flags are denied and never entered OCQ2.
  assert(c.receive(foreign,sizeof(foreign))==Collector::Receive::kDenied);
  assert(c.queued()==0U && f.flash.writes==before);
  frame(blocked,100U,9U);
  blocked[4U]=0U;  // custody intent off
  assert(c.receive(blocked,sizeof(blocked))==Collector::Receive::kDenied);
  assert(f.flash.writes==before);
}

void testFailureAndRevocationFailClosed() {
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  submitFrame(c,1U);
  assert(c.requestFlush());
  uint8_t out[ack::kMaxSize]{};
  memset(out,0xA5U,sizeof(out));
  size_t n=345U;
  f.grants.active=false;
  assert(c.prepare(out,n)==Collector::Prepare::kDenied);
  for (uint8_t x:out) assert(x==0xA5U);
  assert(n==345U && c.queued()==1U && !c.prepared());
  assert(!c.markTxAdmitted());
  f.grants.active=true;
  assert(c.prepare(out,n)==Collector::Prepare::kReady);
  f.grants.active=false;
  // Before actual TX the physical application loop must call prepare()
  // once more, serializing grant mutation and send admission.
  assert(c.prepare(out,n)==Collector::Prepare::kDenied);
  assert(!c.markTxAdmitted());
  f.grants.active=true;
  assert(c.prepare(out,n)==Collector::Prepare::kReady);
  assert(c.markTxAdmitted());

  // Flash failure: never ACK the uncertain second object. The fixture
  // creates a fresh actual OCQ2 store for this independent scenario.
  Fixture damaged;
  Collector failed(damaged.store,damaged.grants,kGateway);
  uint8_t bytes[100]{};
  frame(bytes,100U,1U);
  assert(failed.receive(bytes,sizeof(bytes))==
         Collector::Receive::kStartedFlash);
  damaged.flash.fail_next=true;
  auto result=Collector::Service::kWaitingFlash;
  for (unsigned i=0U;i<32U;++i) {
    result=failed.service();
    if(result!=Collector::Service::kWaitingFlash)break;
  }
  assert(result==Collector::Service::kRejected);
  assert(!failed.pendingFlash() && failed.queued()==0U);
  assert(!failed.requestFlush());
  size_t length=888U;
  assert(failed.prepare(out,length)!=Collector::Prepare::kReady);
  assert(length==888U);
}

void testNoDoubleCountingDuplicateHandle() {
  Fixture f;
  Collector c(f.store,f.grants,kGateway);
  submitFrame(c,1U);
  uint8_t same[100]{};
  frame(same,sizeof(same),1U);
  const size_t before=f.flash.writes;
  assert(c.receive(same,sizeof(same))==Collector::Receive::kDuplicate);
  assert(c.queued()==1U && f.flash.writes==before);
  assert(c.requestFlush());
  uint8_t out[ack::kMaxSize]{};
  size_t len=0U;
  assert(c.prepare(out,len)==Collector::Prepare::kReady && len==56U);
  assert(c.markTxAdmitted());
}

} // namespace

int main() {
  // Parent's real OCQ2 durable readback, corruption and revocation tests.
  kq05_durable_parent_main();
  testFiveFullAndExplicitPartialGroup();
  testNoMixedTrackerOrInvalidCustody();
  testFailureAndRevocationFailClosed();
  testNoDoubleCountingDuplicateHandle();
  puts("KQ05 Gateway FIVE durable OCQ2 handle collector + retry/flush/authorization: PASS (HOST ONLY)");
  puts("NO REAL RF, NO DEVICE FLASH, NO TRACKER WIPES, NO UPLOAD");
}
