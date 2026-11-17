#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "custody_ack_primitives.h"
#include "kq05_edge_link_wire.h"
#include "kq05_gateway_ram_relay.h"

#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
#error "KQ05 RAM relay test is only for isolated LAB"
#endif

namespace relay = orun_tlp::kq05_ram_relay;
namespace wire = orun_tlp::kq05_edge_link;
using orun_tlp::custody_crypto::hmacSha256Bounded;

namespace {
constexpr uint64_t kGateway = UINT64_C(0x4741544557415931); // GATEWAY1
constexpr uint64_t kTracker = UINT64_C(0x545241434b455231); // TRACKER1
constexpr char kDomain[] = "ORUN-KQ05-EDGE-GATEWAY-OFFER-v1";
constexpr char kEdge[] = "edge-one";

void put64(uint64_t value, uint8_t* output) {
  for (int i = 7; i >= 0; --i) {
    output[i] = static_cast<uint8_t>(value);
    value >>= 8U;
  }
}

struct Credentials final : relay::CredentialSource {
  mutable bool verified = true;
  mutable bool allow = true;
  mutable bool link = true;

  bool verifiedForGateway(uint64_t gateway) const override {
    return verified && gateway == kGateway;
  }
  bool readGatewayLink(uint64_t gateway, char (&edge)[65],
                       uint8_t (&key)[32]) const override {
    if (!link || gateway != kGateway) return false;
    memset(edge, 0, sizeof(edge));
    memcpy(edge, kEdge, sizeof(kEdge));
    memset(key, 0x4b, sizeof(key));
    return true;
  }
  bool permitsTracker(uint64_t gateway, uint64_t tracker,
                      uint32_t epoch) const override {
    return allow && gateway == kGateway && tracker == kTracker &&
           epoch == 7U;
  }
};

size_t signedOffer(uint64_t source_row, uint64_t gateway,
                   uint64_t tracker, uint8_t (&wrapped)[160],
                   uint8_t sequence=101U, uint8_t count=1U) {
  assert(count >= 1U && count <= 8U);
  uint8_t raw[123]{};
  const size_t receipt_size = 40U + 4U * count;
  const size_t raw_size = 19U + receipt_size + 32U;
  raw[0] = 1U;
  raw[1] = 0x91U;
  put64(gateway, raw + 2U);
  put64(source_row, raw + 10U);
  raw[18] = static_cast<uint8_t>(receipt_size);
  auto* receipt = raw + 19U;
  receipt[0] = 2U;
  receipt[1] = 9U;
  receipt[2] = 0U;
  receipt[3] = count;
  put64(tracker, receipt + 4U);
  put64(UINT64_C(0x494e4341524e3031), receipt + 12U); // INCARN01
  receipt[23U] = 7U;
  for (uint8_t i = 0U; i < count; ++i)
    receipt[24U + 4U * i + 3U] = static_cast<uint8_t>(sequence + i);
  for (size_t i = 0U; i < 16U; ++i)
    receipt[24U + 4U * count + i] = static_cast<uint8_t>(0x30U + i);

  uint8_t input[200]{};
  const size_t prefix = sizeof(kDomain) + sizeof(kEdge);
  memcpy(input, kDomain, sizeof(kDomain));
  memcpy(input + sizeof(kDomain), kEdge, sizeof(kEdge));
  const size_t unsigned_length = raw_size - 32U;
  memcpy(input + prefix, raw, unsigned_length);
  uint8_t key[32]{};
  memset(key, 0x4b, sizeof(key));
  uint8_t tag[32]{};
  assert(hmacSha256Bounded(
      key, input, prefix + unsigned_length, tag));
  memcpy(raw + raw_size - 32U, tag, sizeof(tag));
  const size_t logical_size =
      wire::wrapOffer(raw, raw_size, wrapped, 160U);
  assert(logical_size == raw_size + 1U);
  return logical_size;
}

relay::TxConditions readyPolicy() {
  relay::TxConditions p{};
  p.gateway_service_authorized = true;
  p.radio_idle = true;
  p.custody_ack_pending = false;
  p.command_pending = false;
  p.live_pending = false;
  p.regional_rf_budget_granted = true;
  p.tracker_rx_window_authorized = true;
  p.heard_tracker = kTracker;
  p.heard_age_ms = 1500U;
  return p;
}

void testAuthScopeAndNoAck() {
  Credentials c{};
  relay::GatewayRamRelay queue(kGateway);
  uint8_t frame[160]{};
  const size_t len = signedOffer(1U, kGateway, kTracker, frame);
  assert(queue.accept(frame, len, 100U, true, c) ==
         relay::Admission::kQueued);
  assert(queue.accept(frame, len, 101U, true, c) ==
         relay::Admission::kDuplicate);
  assert(queue.pending(102U) == 1U); // Never ACKs or deletes Backend data.

  frame[len - 1U] ^= 0x01U;
  assert(queue.accept(frame, len, 102U, true, c) ==
         relay::Admission::kDenied);
  frame[len - 1U] ^= 0x01U;
  frame[30U] ^= 0x01U; // semantic payload / MAC changes
  assert(queue.accept(frame, len, 102U, true, c) ==
         relay::Admission::kDenied);
  frame[30U] ^= 0x01U;

  size_t size = signedOffer(1U, kGateway, kTracker, frame, 102U);
  assert(queue.accept(frame, size, 103U, true, c) ==
         relay::Admission::kDenied); // Same Edge row, different receipt.
  size = signedOffer(2U, UINT64_C(0x4741544557415932),
                     kTracker, frame);
  assert(queue.accept(frame, size, 104U, true, c) ==
         relay::Admission::kDenied);
  size = signedOffer(2U, kGateway, UINT64_C(0x4f54484552444556), frame);
  assert(queue.accept(frame, size, 104U, true, c) ==
         relay::Admission::kDenied);
  size = signedOffer(2U, kGateway, kTracker, frame);
  // An otherwise authentic message for the wrong key epoch is NOT
  // authorized by an older OGK1 grant.
  frame[1U + 19U + 23U] = 8U;
  assert(queue.accept(frame, size, 105U, true, c) ==
         relay::Admission::kDenied);
  size = signedOffer(2U, kGateway, kTracker, frame);
  c.allow = false;
  assert(queue.accept(frame, size, 105U, true, c) ==
         relay::Admission::kDenied);
  c.allow = true;
  c.link = false;
  assert(queue.accept(frame, size, 106U, true, c) ==
         relay::Admission::kDenied);
  c.link = true;
  assert(queue.accept(frame, size, 107U, true, c) ==
         relay::Admission::kQueued);
  assert(queue.pending(107U) == 2U);
  size = signedOffer(3U, kGateway, kTracker, frame);
  assert(queue.accept(frame, size, 108U, true, c) ==
         relay::Admission::kFull);
  assert(queue.pending(108U) == 2U);
}

void testPriorityWindowRateAndExpiration() {
  Credentials c{};
  relay::GatewayRamRelay q(kGateway);
  uint8_t frame[160]{};
  const size_t len = signedOffer(1U, kGateway, kTracker, frame);
  assert(q.accept(frame, len, 100U, true, c) ==
         relay::Admission::kQueued);
  relay::TxConditions p = readyPolicy();
  relay::Candidate out{};
  out.source_row = 999U;
  assert(q.select(150U, p, c, out));
  assert(out.source_row == 1U && out.length == 44U &&
         out.tracker == kTracker);
  assert(q.pending(150U) == 1U); // selection does NOT imply TX.
  p.custody_ack_pending = true;
  assert(!q.select(150U, p, c, out));
  assert(out.source_row == 0U && out.length == 0U);
  p.custody_ack_pending = false;
  p.command_pending = true;
  assert(!q.select(150U, p, c, out));
  p.command_pending = false;
  p.live_pending = true;
  assert(!q.select(150U, p, c, out));
  p.live_pending = false;
  p.regional_rf_budget_granted = false;
  assert(!q.select(150U, p, c, out));
  p.regional_rf_budget_granted = true;
  p.tracker_rx_window_authorized = false;
  assert(!q.select(150U, p, c, out));
  p.tracker_rx_window_authorized = true;
  p.heard_age_ms = 3001U;
  assert(!q.select(150U, p, c, out));
  p.heard_age_ms = 3000U;
  assert(q.select(150U, p, c, out));
  p.radio_idle = false;
  assert(!q.select(150U, p, c, out));
  p.radio_idle = true;
  p.heard_tracker = UINT64_C(0x4f54484552444556);
  assert(!q.select(150U, p, c, out));
  p.heard_tracker = kTracker;
  q.markTxStarted(1U, 150U);
  assert(q.pending(151U) == 1U); // radio start != Tracker HMAC release
  assert(!q.select(200U, p, c, out)); // no immediate duplicate RF attempt
  assert(q.pending(100U + q.kTtlMs) == 0U);

  // Revocation must discard every transient item without a release claim.
  assert(q.accept(frame, len, 21000U, true, c) ==
         relay::Admission::kQueued);
  c.verified = false;
  assert(!q.select(21001U, p, c, out));
  assert(q.pending(21001U) == 0U);
}

void testWrapAndBounds() {
  Credentials c{};
  relay::GatewayRamRelay q(kGateway);
  uint8_t frame[160]{};
  size_t len = signedOffer(7U, kGateway, kTracker, frame, 1U, 8U);
  assert(len == 124U);
  assert(q.accept(frame, len, 400U, true, c) ==
         relay::Admission::kQueued);
  relay::TxConditions p = readyPolicy();
  relay::Candidate out{};
  assert(q.select(401U, p, c, out));
  assert(out.length == 72U && out.source_row == 7U);
  assert(q.accept(frame, len - 1U, 402U, true, c) ==
         relay::Admission::kDenied);
  assert(q.accept(nullptr, len, 402U, true, c) ==
         relay::Admission::kDenied);
  assert(q.accept(frame, len, 403U, false, c) ==
         relay::Admission::kDenied);
  assert(q.pending(403U) == 0U);
}

void testMillisWrap() {
  Credentials c{};
  relay::GatewayRamRelay q(kGateway);
  uint8_t frame[160]{};
  const size_t len = signedOffer(8U, kGateway, kTracker, frame);
  const uint32_t started = UINT32_MAX - 1000U;
  assert(q.accept(frame, len, started, true, c) ==
         relay::Admission::kQueued);
  assert(q.pending(2000U) == 1U);
  assert(q.pending(static_cast<uint32_t>(started + q.kTtlMs)) == 0U);
}
}  // namespace

int main() {
  testAuthScopeAndNoAck();
  testPriorityWindowRateAndExpiration();
  testWrapAndBounds();
  testMillisWrap();
  return 0;
}
