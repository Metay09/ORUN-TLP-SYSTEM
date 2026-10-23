#include "sf5k3_release_progress_candidate.h"
#include <assert.h>
#include <stdio.h>
using namespace orun_tlp::sf5k3_candidate;
namespace {
using ID = osf::RecordIdentity;
ID id(uint32_t n) { ID v{};v.incarnation=17U;v.sequence=n;return v; }
struct Flash : StorePort {
 struct Item {bool released=false,cache=true,match=true,uncertain=false;} items[4]{};
 bool ready=false, success=false, failed_read=false, busy_once=false;
 uint32_t opseq=0, starts=0, clears=0;
 bool inspect(const ID& x, DurableView& out) override {
   if (failed_read || x.incarnation != 17U || x.sequence == 0U || x.sequence > 4U) return false;
   const Item& i=items[x.sequence-1U];
   out.record_present=true;out.released=i.released;out.exact_present=i.cache;
   out.exact_matches=i.match;out.mutation_uncertain=i.uncertain;return true;
 }
 Request requestRelease(const ID& x) override {
   if(busy_once){busy_once=false;return Request::kBusy;}
   if(opseq || items[x.sequence-1U].released)return Request::kRejected;
   opseq=x.sequence;++starts;return Request::kStarted;
 }
 bool takeReleaseResult(bool& ok)override{
   if(!opseq || !ready)return false;
   ok=success;ready=false;opseq=0;return true;
 }
 void complete(bool ok,bool persist=true){
   assert(opseq && !ready);ready=true;success=ok;
   if(ok && persist){items[opseq-1U].released=true;items[opseq-1U].cache=false;}
 }
 void reboot(){ready=false;success=false;opseq=0;busy_once=false;}
};
void drain(ReleaseProgress& c,Flash& f){
 for(int i=0;i<32;++i){auto s=c.tick();if(s==Status::kFinished)return;
   assert(s!=Status::kFault);
   if(f.opseq && !f.ready)f.complete(true);
 }assert(false);
}
void happyAndSelective(){
 Flash f;ID a[4]={id(1),id(2),id(3),id(4)};
 ReleaseProgress c(f);assert(c.beginHostPreflighted(a,4U));drain(c,f);
 assert(f.starts==4 && f.clears==0);
 for(const auto& x:f.items)assert(x.released&&!x.cache);
 Flash p;ID subset[2]={id(1),id(3)};
 ReleaseProgress sel(p);assert(sel.beginHostPreflighted(subset,2U));drain(sel,p);
 assert(p.items[0].released&&p.items[2].released);
 assert(!p.items[1].released&&p.items[1].cache);
 assert(!p.items[3].released&&p.items[3].cache);
}
void recoveryNeedsFreshSubsetProof(){
 Flash f;ID a[4]={id(1),id(2),id(3),id(4)};
 ReleaseProgress c(f);assert(c.beginHostPreflighted(a,4U));
 assert(c.tick()==Status::kAwaitingFlash);
 f.complete(true);f.reboot(); // release persisted, callback lost
 assert(f.items[0].released&&!f.items[0].cache);
 ReleaseProgress stale(f);
 assert(!stale.beginHostPreflighted(a,4U)); // full ACK needs lost ciphertext
 assert(f.starts==1);
 // Gateway issues a NEW authenticated subset ACK for remaining three.
 ID rest[3]={id(2),id(3),id(4)};
 ReleaseProgress recovered(f);
 assert(recovered.beginHostPreflighted(rest,3U));drain(recovered,f);
 assert(f.starts==4);
 for(const auto& x:f.items)assert(x.released&&!x.cache);
}
void storePreflightFailClosed(){
 Flash f;ID a[4]={id(1),id(2),id(3),id(4)};
 f.items[2].cache=false;
 ReleaseProgress x(f);assert(!x.beginHostPreflighted(a,4U));
 assert(f.starts==0);
 Flash m;m.items[3].match=false;
 ReleaseProgress mm(m);assert(!mm.beginHostPreflighted(a,4U));
 Flash unc;unc.items[1].uncertain=true;
 ReleaseProgress uu(unc);assert(!uu.beginHostPreflighted(a,4U));
 Flash bad;bad.failed_read=true;
 ReleaseProgress bb(bad);assert(!bb.beginHostPreflighted(a,4U));
 Flash dup;ID ids[2]={id(1),id(1)};
 ReleaseProgress d(dup);assert(!d.beginHostPreflighted(ids,2U));
 Flash inv;ID zero{};ReleaseProgress z(inv);assert(!z.beginHostPreflighted(&zero,1U));
}
void callbackFailClosedAndBusy(){
 Flash f;ID one=id(1);ReleaseProgress c(f);
 assert(c.beginHostPreflighted(&one,1U));
 f.busy_once=true;assert(c.tick()==Status::kBusy);
 assert(c.tick()==Status::kAwaitingFlash);
 assert(c.tick()==Status::kAwaitingFlash);
 f.complete(true,false);assert(c.tick()==Status::kWorking);
 assert(c.tick()==Status::kFault); // callback but no durable readback
 Flash failed;ReleaseProgress ff(failed);
 assert(ff.beginHostPreflighted(&one,1U));
 assert(ff.tick()==Status::kAwaitingFlash);
 failed.complete(false);assert(ff.tick()==Status::kFault);
 assert(!failed.items[0].released);
}
}
int main(){
 happyAndSelective();recoveryNeedsFreshSubsetProof();storePreflightFailClosed();
 callbackFailClosedAndBusy();
 puts("SF5K3A actual-store-compatible release + new subset ACK recovery: PASS");
}
