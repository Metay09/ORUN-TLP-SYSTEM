// KQ05 Gateway-local selective custody ACK envelope; PC HOST ONLY.
// Structural tests do NOT check HMAC, authorize ACKs or release storage.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_gateway_batch_custody_ack.h"

namespace ack = orun_tlp::kq05_gateway_batch_ack;
namespace {

ack::Header sample(uint8_t count) {
  ack::Header h{};
  h.tracker_device_id = UINT64_C(0x0123456789ABCDEF);
  h.gateway_device_id = UINT64_C(0x4757415949443031);
  h.key_epoch = 7U;
  h.custody_floor = 42U;
  h.grant_generation = 5U;
  h.count = count;
  for (uint8_t i = 0U; i < count; ++i)
    for (uint8_t j = 0U; j < ack::kRefSize; ++j)
      h.refs[i][j] = static_cast<uint8_t>((i + 1U) * 16U + j);
  return h;
}

void noMutationOnEncodeFailure(const ack::Header& bad) {
  uint8_t out[ack::kMaxSize]{};
  memset(out, 0xA5U, sizeof(out));
  uint8_t tag[ack::kTagSize]{};
  size_t written = 400U;
  assert(!ack::encode(bad, tag, out, sizeof(out), written));
  assert(written == 400U);
  for (uint8_t byte : out) assert(byte == 0xA5U);
}

void testRoundTripAndNoExtraBytes() {
  uint8_t tag[ack::kTagSize]{};
  for (size_t i = 0U; i < sizeof(tag); ++i) tag[i] = uint8_t(0xE0U + i);
  assert(ack::kMaxSize == 88U);
  for (uint8_t count = 1U; count <= ack::kMaxObjects; ++count) {
    const ack::Header original = sample(count);
    uint8_t out[ack::kMaxSize + 8U]{};
    memset(out, 0xA5U, sizeof(out));
    size_t written = 0U;
    const size_t expected =
        ack::kHeaderSize + ack::kRefSize*count + ack::kTagSize;
    assert(ack::encode(original, tag, out, sizeof(out), written));
    assert(written == expected);
    for (size_t i = expected; i < sizeof(out); ++i)
      assert(out[i] == 0xA5U);
    ack::Header decoded{};
    assert(ack::decode(out, written, decoded));
    assert(decoded.count == count);
    assert(decoded.tracker_device_id == original.tracker_device_id);
    assert(decoded.gateway_device_id == original.gateway_device_id);
    assert(decoded.key_epoch == original.key_epoch);
    assert(decoded.custody_floor == original.custody_floor);
    assert(decoded.grant_generation == original.grant_generation);
    assert(memcmp(decoded.refs, original.refs, sizeof(decoded.refs)) == 0);
    assert(memcmp(out + written-ack::kTagSize, tag, sizeof(tag)) == 0);

    ack::Header untouched = sample(1U);
    untouched.count = 4U;
    assert(!ack::decode(out, written-1U, untouched));
    assert(untouched.count == 4U);
    assert(untouched.tracker_device_id == original.tracker_device_id);
    assert(!ack::decode(out, written+1U, untouched));
    assert(untouched.count == 4U);

    uint8_t old = out[0];
    out[0] = 1U; // Wrong version.
    assert(!ack::decode(out, written, untouched));
    out[0] = old;
    old = out[1];
    out[1] = 9U; // Backend DURABLE receipt must NOT parse as Gateway ACK.
    assert(!ack::decode(out, written, untouched));
    out[1] = 5U; // Existing one-object custody ACK must remain distinct.
    assert(!ack::decode(out, written, untouched));
    out[1] = old;
    old = out[2];
    out[2] = 0U; // Relay flag is mandatory.
    assert(!ack::decode(out, written, untouched));
    out[2] = 3U; // Unknown flags are forbidden.
    assert(!ack::decode(out, written, untouched));
    out[2] = old;
    assert(ack::decode(out, written, untouched));
  }
}

void testMalformedGroupsRejected() {
  const ack::Header good = sample(5U);
  ack::Header bad = good;
  bad.count = 0U;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.count = 6U;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.tracker_device_id = 0U;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.gateway_device_id = 0U;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.key_epoch = UINT32_MAX;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.custody_floor = UINT32_MAX;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.grant_generation = 0U;
  noMutationOnEncodeFailure(bad);
  bad = good;
  bad.grant_generation = UINT32_MAX;
  noMutationOnEncodeFailure(bad);
  bad = good;
  memcpy(bad.refs[1], bad.refs[0], ack::kRefSize);
  noMutationOnEncodeFailure(bad);
  bad = good;
  memcpy(bad.refs[0], bad.refs[4], ack::kRefSize);
  noMutationOnEncodeFailure(bad);
  bad = sample(1U);
  bad.refs[4][0] = 1U; // Hidden reference outside declared count.
  noMutationOnEncodeFailure(bad);

  uint8_t buffer[ack::kMaxSize]{};
  memset(buffer, 0x44U, sizeof(buffer));
  uint8_t tag[ack::kTagSize]{};
  size_t written = 1234U;
  assert(!ack::encode(good, tag, buffer, ack::kMinSize, written));
  assert(written == 1234U);
  for (uint8_t byte : buffer) assert(byte == 0x44U);
  assert(!ack::decode(nullptr, ack::kMaxSize, bad));
  assert(bad.count == 1U); // unchanged after null decode failure
}

void testDuplicatedAndReorderedWireReferencesFail() {
  const ack::Header good = sample(5U);
  uint8_t wire[ack::kMaxSize]{};
  uint8_t tag[ack::kTagSize]{};
  size_t written = 0U;
  assert(ack::encode(good, tag, wire, sizeof(wire), written));
  ack::Header out{};
  assert(ack::decode(wire, written, out));
  memcpy(wire + ack::kHeaderSize + ack::kRefSize,
         wire + ack::kHeaderSize, ack::kRefSize);
  assert(!ack::decode(wire, written, out));
  assert(out.count == 5U);
  assert(ack::encode(good, tag, wire, sizeof(wire), written));
  uint8_t first[ack::kRefSize]{};
  memcpy(first, wire + ack::kHeaderSize, sizeof(first));
  memcpy(wire + ack::kHeaderSize,
         wire + ack::kHeaderSize + ack::kRefSize, sizeof(first));
  memcpy(wire + ack::kHeaderSize + ack::kRefSize, first, sizeof(first));
  assert(!ack::decode(wire, written, out));
}

} // namespace

int main() {
  testRoundTripAndNoExtraBytes();
  testMalformedGroupsRejected();
  testDuplicatedAndReorderedWireReferencesFail();
  puts("KQ05 Gateway LOCAL 1..5 selective batch ACK canonical wire: PASS (HOST PARSER ONLY)");
  puts("NO AUTHENTICATED ACK, NO FLASH RELEASE, NO REAL RF, NO DEVICE UPLOAD");
  return 0;
}
