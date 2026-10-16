#include "sf5k2_gateway_batch_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace orun_tlp::sf5k2_candidate;
namespace {
const uint64_t kTracker = 0x0102030405060708ULL;
const uint64_t kGateway = 0x1112131415161718ULL;
Scope scope() {
  Scope s{};
  s.tracker_id = kTracker; s.gateway_id = kGateway;
  s.key_epoch = 7U; s.policy_floor = 8U; s.grant_generation = 9U;
  return s;
}
void be64(uint8_t* out, uint64_t v) {
  for (size_t i = 0; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(v >> (56U - 8U * i));
}
uint8_t blobs[6][rec::kObjectMax]{};
void init() {
  for (size_t i = 0; i < 6U; ++i) {
    const size_t n = (i % 2U) ? 84U : 100U;
    for (size_t j = 0; j < n; ++j)
      blobs[i][j] = static_cast<uint8_t>(i * 11U + j * 3U);
    blobs[i][0] = 2U; blobs[i][1] = 4U; blobs[i][2] = 2U;
    blobs[i][3] = n == 100U ? 1U : 2U;
    blobs[i][4] = 3U; blobs[i][5] = 1U;
    blobs[i][6] = n == 100U ? 64U : 48U;
    blobs[i][7] = 0U;
    be64(blobs[i] + 8, kTracker);
    blobs[i][16] = 0U; blobs[i][17] = 0U;
    blobs[i][18] = 0U; blobs[i][19] = 7U;
    be64(blobs[i] + 20, i + 1U);
  }
}
Pending pending(size_t i) {
  Pending p{}; p.exact = blobs[i]; p.size = i % 2U ? 84U : 100U;
  return p;
}
struct Mock : EvidencePort {
  bool present[kWindow]{};
  uint8_t stored[kWindow][rec::kRecordSize]{};
  EdgeFact edge[kWindow]{};
  size_t local_calls = 0U;
  size_t edge_calls = 0U;
  Mock() { for (size_t i = 0; i < kWindow; ++i) edge[i] = EdgeFact::kUnavailable; }
  void store(size_t at, const Pending& p) {
    assert(at < kWindow);
    assert(rec::encodeStagedRecord(p.exact, p.size, stored[at], rec::kRecordSize));
    memset(stored[at] + rec::kRecordCommitOffset, 0, 4);
    present[at] = true;
  }
  bool readLocalCandidate(size_t index, uint8_t out[rec::kRecordSize]) override {
    ++local_calls;
    if (!present[index]) return false;
    memcpy(out, stored[index], rec::kRecordSize);
    return true;
  }
  EdgeFact edgeFact(size_t index, const Scope&, const uint8_t*, size_t) override {
    ++edge_calls;
    return edge[index];
  }
};
void completeWindowAndBacklog() {
  Pending first[4] = {pending(0), pending(1), pending(2), pending(3)};
  Mock all{}; for (size_t i = 0; i < 4U; ++i) all.store(i, first[i]);
  PreparedCandidate out{};
  assert(prepare(scope(), first, 4, all, &out) == Result::kPrepared);
  assert(out.count == 4U && all.local_calls == 4U && all.edge_calls == 0U);
  for (size_t i = 0; i < 4U; ++i) {
    assert(out.members[i].input_index == i);
    assert(out.members[i].size == first[i].size);
    assert(memcmp(out.members[i].bytes, first[i].exact, first[i].size) == 0);
  }
  Pending last[2] = {pending(4), pending(5)};
  Mock tail{}; tail.store(0, last[0]); tail.store(1, last[1]);
  assert(prepare(scope(), last, 2, tail, &out) == Result::kPrepared);
  assert(out.count == 2 && out.members[0].input_index == 0);
  // Four is only the window bound, not the logical history capacity.
}
void selectiveAndDuplicate() {
  Pending list[4] = {pending(0), pending(1), pending(2), pending(3)};
  Mock partial{}; partial.store(0, list[0]); partial.store(2, list[2]);
  PreparedCandidate out{};
  assert(prepare(scope(), list, 4, partial, &out) == Result::kPrepared);
  assert(out.count == 2 && out.members[0].input_index == 0 &&
         out.members[1].input_index == 2);
  Mock again{}; again.store(0, list[0]); again.store(2, list[2]);
  PreparedCandidate duplicate{};
  assert(prepare(scope(), list, 4, again, &duplicate) == Result::kPrepared);
  assert(memcmp(out.members, duplicate.members, sizeof(out.members)) == 0);
  // Duplicate delivery is idempotent staging only, never duplicate release.
  list[3] = list[0];
  const size_t previous_calls = again.local_calls;
  assert(prepare(scope(), list, 4, again, &out) == Result::kInvalid);
  assert(again.local_calls == previous_calls); // no evidence check on malformed group
}
void uncertainNoSoftAck() {
  Pending list[4] = {pending(0), pending(1), pending(2), pending(3)};
  Mock evidence{};
  for (size_t i = 0; i < 4U; ++i) evidence.store(i, list[i]);
  // Staged, partial commit, wrong object and torn/corrupt body cannot qualify.
  memset(evidence.stored[0] + rec::kRecordCommitOffset, 0xFF, 4);
  evidence.stored[1][rec::kRecordCommitOffset] = 0xFE;
  evidence.stored[2][rec::kRecordObjectOffset + 8] ^= 1U;
  evidence.stored[3][rec::kRecordCrcOffset] ^= 0x80U;
  PreparedCandidate out{}; out.count = 99U;
  assert(prepare(scope(), list, 4, evidence, &out) == Result::kNoDurableFact);
  assert(out.count == 99U); // unchanged on no proof
  Mock empty{}; empty.edge[0] = EdgeFact::kTransportOnly;
  assert(prepare(scope(), list, 1, empty, &out) == Result::kNoDurableFact);
  assert(out.count == 99U);
  // Mock verified Edge fact accepted only when no uncertain local fallback.
  empty.edge[0] = EdgeFact::kAuthenticatedDurableExact;
  Pending uncertain = list[0]; uncertain.local_mutation_started = true;
  const size_t calls = empty.edge_calls;
  assert(prepare(scope(), &uncertain, 1, empty, &out) == Result::kNoDurableFact);
  assert(empty.edge_calls == calls);
  // When a local commit really exists, its independent readback is eligible.
  empty.store(0, uncertain);
  assert(prepare(scope(), &uncertain, 1, empty, &out) == Result::kPrepared);
  assert(out.count == 1U);
  Mock edgeOnly{}; edgeOnly.edge[0] = EdgeFact::kAuthenticatedDurableExact;
  assert(prepare(scope(), list, 1, edgeOnly, &out) == Result::kPrepared);
  assert(out.count == 1U);
}
void rejectIncorrectIdentity() {
  Pending p = pending(0);
  Mock evidence{}; evidence.store(0, p);
  PreparedCandidate out{}; out.count = 83U;
  Scope s = scope(); s.tracker_id++;
  assert(prepare(s, &p, 1, evidence, &out) == Result::kInvalid);
  s = scope(); s.key_epoch++;
  assert(prepare(s, &p, 1, evidence, &out) == Result::kInvalid);
  s = scope(); s.grant_generation = 0U;
  assert(prepare(s, &p, 1, evidence, &out) == Result::kInvalid);
  s = scope(); s.gateway_id = 0U;
  assert(prepare(s, &p, 1, evidence, &out) == Result::kInvalid);
  assert(out.count == 83U && evidence.local_calls == 0U);
  p = pending(0); p.size = 30U;
  assert(prepare(scope(), &p, 1, evidence, &out) == Result::kInvalid);
  p = pending(0); p.exact = nullptr;
  assert(prepare(scope(), &p, 1, evidence, &out) == Result::kInvalid);
  p = pending(0); blobs[0][4] = 0U;
  assert(prepare(scope(), &p, 1, evidence, &out) == Result::kInvalid);
  blobs[0][4] = 3U;
  assert(prepare(scope(), &p, 0, evidence, &out) == Result::kInvalid);
  Pending many[5] = {pending(0),pending(1),pending(2),pending(3),pending(4)};
  assert(prepare(scope(), many, 5, evidence, &out) == Result::kInvalid);
  assert(prepare(scope(), &p, 1, evidence, nullptr) == Result::kInvalid);
}
} // namespace
int main() {
  init();
  completeWindowAndBacklog();
  selectiveAndDuplicate();
  uncertainNoSoftAck();
  rejectIncorrectIdentity();
  puts("SF5K2 host-only Gateway durable-evidence batch staging: PASS");
}
