#include "sf5k1_batch_ack_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace orun_tlp::sf5k1_candidate;

namespace {
CandidateAck sample(unsigned n) {
  CandidateAck a;
  a.flags = kRelayAllowed;
  a.count = static_cast<uint8_t>(n);
  a.tracker_id = 0x0102030405060708ULL;
  a.gateway_id = 0x1112131415161718ULL;
  a.key_epoch = 0x12345678U;
  a.custody_policy_floor = 0x90ABCDEFU;
  a.custody_grant_generation = 0x10203040U;
  for (unsigned i = 0; i < n; ++i) {
    a.entries[i].object_len = (i < 2U) ? 84U : 100U;
    a.entries[i].object_ref = static_cast<uint64_t>(i + 1U);
  }
  for (size_t i = 0; i < kTagSize; ++i)
    a.opaque_mac[i] = static_cast<uint8_t>(i ^ 0x5AU);
  return a;
}
void rounds() {
  for (unsigned n = 1; n <= 4; ++n) {
    const CandidateAck a = sample(n);
    const size_t length = kPrefixSize + n * kEntrySize + kTagSize;
    uint8_t wire[kMaxSize];
    memset(wire, 0xCC, sizeof(wire));
    assert(encode(a, wire, length));
    assert(wire[0] == 2 && wire[1] == 6 && wire[2] == 1 && wire[3] == n);
    assert(wire[4] == 1 && wire[11] == 8 && wire[12] == 0x11);
    assert(wire[20] == 0x12 && wire[23] == 0x78);
    assert(wire[kPrefixSize] == 84);
    CandidateAck parsed;
    assert(decode(wire, length, &parsed));
    assert(parsed.count == n && parsed.tracker_id == a.tracker_id);
    assert(parsed.gateway_id == a.gateway_id &&
           parsed.custody_policy_floor == a.custody_policy_floor);
    assert(parsed.custody_grant_generation == a.custody_grant_generation);
    for (unsigned i = 0; i < n; ++i) {
      assert(parsed.entries[i].object_len == a.entries[i].object_len);
      assert(parsed.entries[i].object_ref == a.entries[i].object_ref);
    }
    assert(memcmp(parsed.opaque_mac, a.opaque_mac, kTagSize) == 0);
    uint8_t duplicate[kMaxSize]{};
    assert(encode(parsed, duplicate, length));
    assert(memcmp(duplicate, wire, length) == 0);
    assert(!encode(a, duplicate, length - 1U));
    assert(!decode(wire, length - 1U, &parsed));
    assert(!decode(wire, length + 1U, &parsed));
  }
}
void rejection() {
  CandidateAck a = sample(4);
  uint8_t valid[kMaxSize]{};
  assert(encode(a, valid, sizeof(valid)));
  auto invalid = [&](size_t at, uint8_t value) {
    uint8_t changed[kMaxSize];
    memcpy(changed, valid, sizeof(changed));
    changed[at] = value;
    CandidateAck parsed;
    parsed.tracker_id = 0xDEADBEEFULL;
    assert(!decode(changed, sizeof(changed), &parsed));
    assert(parsed.tracker_id == 0xDEADBEEFULL);
  };
  invalid(0, 1); invalid(1, 5); invalid(2, 2);
  invalid(3, 0); invalid(3, 5); invalid(3, 3);
  invalid(32, 85); // invalid object length
  invalid(32 + 9 + 8, 1); // duplicate ref in second 84B entry
  invalid(32 + 3 * 9, 84); // 84 after preceding 100B entry
  a.count = 0;
  memset(valid, 0xB7, sizeof(valid));
  assert(!encode(a, valid, sizeof(valid)) && valid[0] == 0xB7);
  a = sample(4); a.entries[1].object_ref = a.entries[0].object_ref;
  assert(!encode(a, valid, sizeof(valid)) && valid[0] == 0xB7);
  a = sample(4); a.flags = 2;
  assert(!encode(a, valid, sizeof(valid)) && valid[0] == 0xB7);
  assert(!decode(nullptr, kMaxSize, &a));
  assert(!decode(valid, kMaxSize, nullptr));
  assert(!encode(a, nullptr, kMaxSize));
  // Structural parser deliberately cannot authenticate. Altering tag bytes
  // must remain structurally valid: no consumer may call this custody proof.
  a = sample(4);
  assert(encode(a, valid, sizeof(valid)));
  valid[kMaxSize - 1U] ^= 1U;
  CandidateAck parsed;
  assert(decode(valid, sizeof(valid), &parsed));
  assert(parsed.opaque_mac[kTagSize - 1U] != a.opaque_mac[kTagSize - 1U]);
}
} // namespace

int main() {
  rounds();
  rejection();
  puts("SF5K1 host-only candidate batch ACK structural codec: PASS");
}
