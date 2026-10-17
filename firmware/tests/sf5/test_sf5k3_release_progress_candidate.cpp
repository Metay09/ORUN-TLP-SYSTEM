#include "sf5k3_release_progress_candidate.h"
#include <assert.h>
#include <stdio.h>

using namespace orun_tlp::sf5k3_candidate;
namespace {
using ID = osf::RecordIdentity;
ID identity(uint32_t n) { ID id{}; id.incarnation = 17U; id.sequence = n; return id; }

struct FakeStore : StorePort {
  struct Item { bool present = true; bool released = false; bool cache = true;
                bool match = true; bool uncertain = false; } items[4]{};
  enum class Op { kNone, kRelease, kClear } op = Op::kNone;
  bool ready = false, ok = false, fail_read = false, busy_once = false;
  uint32_t target = 0U, releases = 0U, clears = 0U;
  uint32_t forbidden_early_clear = 0U;
  bool inspect(const ID& id, DurableView& out) override {
    if (fail_read || id.incarnation != 17U || id.sequence < 1U || id.sequence > 4U)
      return false;
    const Item& s = items[id.sequence-1U];
    out.record_present = s.present; out.released = s.released;
    out.exact_present = s.cache; out.exact_matches = s.match;
    out.mutation_uncertain = s.uncertain;
    return true;
  }
  Request requestRelease(const ID& id) override {
    if (busy_once) { busy_once = false; return Request::kBusy; }
    if (op != Op::kNone || items[id.sequence-1U].released) return Request::kRejected;
    op = Op::kRelease; target = id.sequence; ++releases; return Request::kStarted;
  }
  Request requestClearExact(const ID& id) override {
    if (busy_once) { busy_once = false; return Request::kBusy; }
    if (!items[id.sequence-1U].released) { ++forbidden_early_clear; return Request::kRejected; }
    if (op != Op::kNone || !items[id.sequence-1U].cache) return Request::kRejected;
    op = Op::kClear; target = id.sequence; ++clears; return Request::kStarted;
  }
  bool takeReleaseResult(bool& success) override {
    if (op != Op::kRelease || !ready) return false;
    success = ok; ready = false; op = Op::kNone; return true;
  }
  bool takeClearResult(bool& success) override {
    if (op != Op::kClear || !ready) return false;
    success = ok; ready = false; op = Op::kNone; return true;
  }
  // Complete operation: persists durable state *before* result callback.
  void complete(bool success, bool persist = true) {
    assert(op != Op::kNone && !ready);
    ok = success;
    if (success && persist) {
      Item& i = items[target-1U];
      if (op == Op::kRelease) i.released = true;
      else if (op == Op::kClear) i.cache = false;
    }
    ready = true;
  }
  // Models reboot: only persistent Item fields survive, lost callback gone.
  void reboot() { op = Op::kNone; ready = false; ok = false; busy_once = false; }
};

void drain(ReleaseProgress& c, FakeStore& flash) {
  for (int i = 0; i < 64; ++i) {
    const Status s = c.tick();
    if (s == Status::kFinished) return;
    assert(s != Status::kFault);
    if (flash.op != FakeStore::Op::kNone && !flash.ready) flash.complete(true);
  }
  assert(false && "never reached completion");
}

void fourAndSelective() {
  FakeStore f;
  ID all[4] = {identity(1), identity(2), identity(3), identity(4)};
  ReleaseProgress c(f);
  assert(c.beginHostPreflighted(all, 4U));
  drain(c, f);
  assert(c.completed() == 4U && f.releases == 4U && f.clears == 4U);
  for (const auto& item: f.items) assert(item.released && !item.cache);
  assert(f.forbidden_early_clear == 0U);
  // Same authenticated ACK replayed after a simulated reboot: no flash wear.
  f.reboot(); ReleaseProgress after(f);
  assert(after.beginHostPreflighted(all, 4U)); drain(after, f);
  assert(f.releases == 4U && f.clears == 4U);

  FakeStore partial;
  ID subset[2] = {identity(1), identity(3)};
  ReleaseProgress p(partial);
  assert(p.beginHostPreflighted(subset, 2U)); drain(p, partial);
  assert(partial.items[0].released && partial.items[2].released);
  assert(!partial.items[1].released && partial.items[1].cache);
  assert(!partial.items[3].released && partial.items[3].cache);
}

void crashAfterReleaseAndLostCallback() {
  FakeStore f; const ID id = identity(1);
  ReleaseProgress c(f);
  assert(c.beginHostPreflighted(&id, 1U));
  assert(c.tick() == Status::kAwaitingFlash);
  f.complete(true); // Flash commit/readback succeeds but power cuts before callback.
  f.reboot();
  assert(f.items[0].released && f.items[0].cache);
  ReleaseProgress recovered(f);
  assert(recovered.beginHostPreflighted(&id, 1U));
  drain(recovered, f);
  assert(f.releases == 1U && f.clears == 1U && !f.items[0].cache);
}

void interruptionBeforeCommitAndAfterClear() {
  FakeStore f; const ID id = identity(1);
  ReleaseProgress c(f); assert(c.beginHostPreflighted(&id, 1U));
  assert(c.tick() == Status::kAwaitingFlash);
  f.reboot(); // no durable release
  ReleaseProgress retry(f); assert(retry.beginHostPreflighted(&id, 1U));
  assert(retry.tick() == Status::kAwaitingFlash);
  f.complete(true); assert(retry.tick() == Status::kWorking);
  assert(retry.tick() == Status::kWorking); // first release pass advanced
  assert(retry.tick() == Status::kWorking); // transition all-released -> clear
  assert(retry.tick() == Status::kAwaitingFlash); // now clear requested
  f.complete(true); f.reboot(); // clear committed; callback lost
  ReleaseProgress after(f); assert(after.beginHostPreflighted(&id, 1U));
  drain(after, f);
  assert(f.items[0].released && !f.items[0].cache);
  assert(f.clears == 1U);
}

void faultsNeverClearWithoutRelease() {
  const ID id = identity(1);
  FakeStore bad;
  bad.items[0].cache = false;
  ReleaseProgress c(bad);
  assert(c.beginHostPreflighted(&id, 1U));
  assert(c.tick() == Status::kFault);
  assert(bad.releases == 0 && bad.clears == 0);
  FakeStore mismatch; mismatch.items[0].match = false;
  ReleaseProgress m(mismatch); assert(m.beginHostPreflighted(&id,1U));
  assert(m.tick() == Status::kFault && mismatch.releases == 0);
  FakeStore uncertain; uncertain.items[0].uncertain = true;
  ReleaseProgress u(uncertain); assert(u.beginHostPreflighted(&id,1U));
  assert(u.tick() == Status::kFault && uncertain.releases == 0);
  FakeStore read; read.fail_read = true;
  ReleaseProgress r(read); assert(r.beginHostPreflighted(&id,1U));
  assert(r.tick() == Status::kFault && read.releases == 0);
  FakeStore fail;
  ReleaseProgress start(fail); assert(start.beginHostPreflighted(&id,1U));
  assert(start.tick() == Status::kAwaitingFlash);
  assert(start.tick() == Status::kAwaitingFlash); // no callback, no mutation
  fail.complete(false);
  assert(start.tick() == Status::kFault && fail.clears == 0);
  // A callback claiming success without durable readback must fail closed.
  FakeStore lying; ReleaseProgress l(lying);
  assert(l.beginHostPreflighted(&id, 1U));
  assert(l.tick() == Status::kAwaitingFlash);
  lying.complete(true, false);
  assert(l.tick() == Status::kWorking);
  assert(l.tick() == Status::kFault);
  assert(lying.clears == 0U);
  FakeStore clear_failure;
  ReleaseProgress cf(clear_failure);
  assert(cf.beginHostPreflighted(&id, 1U));
  assert(cf.tick() == Status::kAwaitingFlash);
  clear_failure.complete(true);
  assert(cf.tick() == Status::kWorking);
  assert(cf.tick() == Status::kWorking); // release pass advanced
  assert(cf.tick() == Status::kWorking); // all released, enter clear pass
  assert(cf.tick() == Status::kAwaitingFlash);
  clear_failure.complete(false);
  assert(cf.tick() == Status::kFault);
  assert(clear_failure.items[0].released && clear_failure.items[0].cache);
  clear_failure.reboot();
  ReleaseProgress resume(clear_failure);
  assert(resume.beginHostPreflighted(&id,1U)); drain(resume,clear_failure);
  assert(clear_failure.releases == 1U && clear_failure.clears == 2U);
}

void busyAndInvalid() {
  FakeStore f; const ID id = identity(1);
  ReleaseProgress c(f);
  assert(!c.beginHostPreflighted(nullptr, 1U));
  assert(!c.beginHostPreflighted(&id, 0U));
  ID bad[5] = {id,identity(2),identity(3),identity(4),identity(1)};
  assert(!c.beginHostPreflighted(bad, 5U));
  assert(!c.beginHostPreflighted(bad, 2U*0U));
  ID dup[2] = {id,id};
  assert(!c.beginHostPreflighted(dup,2U));
  ID zero{}; assert(!c.beginHostPreflighted(&zero,1U));
  assert(c.beginHostPreflighted(&id,1U));
  assert(!c.beginHostPreflighted(&id,1U));
  f.busy_once = true;
  assert(c.tick() == Status::kBusy && f.releases == 0U);
  drain(c, f);
  assert(f.forbidden_early_clear == 0U);
}

void fourMemberReleaseThenClearCrashRecovery() {
  FakeStore f;
  ID ids[4] = {identity(1), identity(2), identity(3), identity(4)};
  ReleaseProgress first(f); assert(first.beginHostPreflighted(ids, 4U));
  // Crash while releasing the second member: none of the ciphertext slots
  // have been cleared, so the full batch can be reauthenticated.
  assert(first.tick() == Status::kAwaitingFlash);
  f.complete(true);
  assert(first.tick() == Status::kWorking);
  assert(first.tick() == Status::kWorking);
  assert(first.tick() == Status::kAwaitingFlash);
  assert(f.releases == 2U && f.clears == 0U);
  f.reboot();
  for (const auto& item : f.items) assert(item.cache);
  ReleaseProgress resumed(f); assert(resumed.beginHostPreflighted(ids, 4U));
  // Re-run release pass; first release is an idempotent persisted fact.
  for (int i = 0; i < 64 && f.clears == 0U; ++i) {
    const Status s = resumed.tick();
    assert(s != Status::kFault && s != Status::kFinished);
    if (f.op != FakeStore::Op::kNone && !f.ready) f.complete(true);
  }
  assert(f.releases == 5U && f.clears == 1U);
  for (const auto& item : f.items) assert(item.released);
  f.reboot(); // One cache already cleared; the full ACK cannot be re-preflighted.
  ReleaseProgress clean(f);
  assert(clean.beginReleasedCleanup(ids, 4U)); // Released markers alone.
  drain(clean, f);
  assert(f.releases == 5U && f.clears == 4U);
  for (const auto& item : f.items) assert(item.released && !item.cache);
  f.reboot();
  ReleaseProgress again(f); assert(again.beginReleasedCleanup(ids,4U));
  drain(again,f);
  assert(f.releases == 5U && f.clears == 4U); // No extra flash wear.
}

void cleanupRequiresAllDurableReleaseMarkers() {
  FakeStore f;
  ID ids[4] = {identity(1), identity(2), identity(3), identity(4)};
  f.items[0].released = true;
  ReleaseProgress c(f);
  assert(!c.beginReleasedCleanup(ids, 4U)); // No partial group cleanup.
  assert(c.status() == Status::kFault && f.clears == 0U);
  // A single independently released record may be cleaned on its own.
  ReleaseProgress single(f); assert(single.beginReleasedCleanup(ids, 1U));
  drain(single, f);
  assert(f.items[0].released && !f.items[0].cache);
  assert(!f.items[1].released && f.items[1].cache);
  FakeStore uncertain;
  uncertain.items[0].released = true;
  uncertain.items[0].uncertain = true;
  ReleaseProgress refused(uncertain);
  assert(!refused.beginReleasedCleanup(ids, 1U));
  assert(uncertain.clears == 0U);
  FakeStore mismatched;
  mismatched.items[0].released = true;
  mismatched.items[0].match = false;
  ReleaseProgress refusedMismatch(mismatched);
  assert(!refusedMismatch.beginReleasedCleanup(ids, 1U));
  assert(mismatched.clears == 0U);
}
} // namespace
int main() {
  fourMemberReleaseThenClearCrashRecovery();
  cleanupRequiresAllDurableReleaseMarkers();
  fourAndSelective();
  crashAfterReleaseAndLostCallback();
  interruptionBeforeCommitAndAfterClear();
  faultsNeverClearWithoutRelease();
  busyAndInvalid();
  puts("SF5K3A host-only two-pass release/clear crash progress: PASS");
}
