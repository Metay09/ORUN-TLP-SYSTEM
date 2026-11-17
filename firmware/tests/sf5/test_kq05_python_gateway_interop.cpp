// KQ05 LAB: Python real Backend->Edge offer -> actual C++ SF5L6
// reassembler -> GatewayRamRelay HMAC/scoped selection. No live BLE/RF.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "edge_link.h"
#include "kq05_edge_link_wire.h"
#include "kq05_gateway_ram_relay.h"

#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
#error "KQ05 isolated host test only"
#endif

namespace link = orun_tlp::edge_link;
namespace relay = orun_tlp::kq05_ram_relay;
namespace wire = orun_tlp::kq05_edge_link;

namespace {
constexpr uint64_t kGateway = UINT64_C(0x4741544557415931);
constexpr uint64_t kTracker = UINT64_C(0x1122334455667788);
constexpr uint32_t kEpoch = UINT32_C(0x01020304);
// PUBLIC TEST VECTOR; never a live Edge enrollment secret.
constexpr uint8_t kPublicDummyLink[32] = {
    0xcf, 0xa9, 0x87, 0xef, 0x23, 0x9b, 0x76, 0x3e,
    0x3d, 0xc1, 0xb0, 0x5d, 0x3f, 0x7b, 0xd3, 0xbd,
    0xa8, 0xc6, 0x75, 0x65, 0x9a, 0xb0, 0xc1, 0x93,
    0x5d, 0x38, 0x6e, 0x1e, 0x46, 0x6b, 0xa8, 0x4f};

struct FakeSignedEnrollment final : relay::CredentialSource {
  bool signed_snapshot = true;
  bool scope = true;
  bool verifiedForGateway(uint64_t g) const override {
    return signed_snapshot && g == kGateway;
  }
  bool readGatewayLink(uint64_t g, char (&name)[65],
                       uint8_t (&key)[32]) const override {
    if (!verifiedForGateway(g)) return false;
    memset(name, 0, sizeof(name));
    memcpy(name, "edge-one", 8);
    memcpy(key, kPublicDummyLink, sizeof(key));
    return true;
  }
  bool permitsTracker(uint64_t g, uint64_t t,
                      uint32_t epoch) const override {
    return scope && verifiedForGateway(g) &&
           t == kTracker && epoch == kEpoch;
  }
};

size_t readFixture(const char* path, uint8_t (&bytes)[160]) {
  FILE* file = fopen(path, "rb");
  assert(file != nullptr);
  size_t n = fread(bytes, 1, sizeof(bytes), file);
  assert(!ferror(file));
  assert(fgetc(file) == EOF && !ferror(file));
  assert(fclose(file) == 0);
  return n;
}

void testInterop(const uint8_t* frame, size_t n) {
  assert(n == 96U);
  link::Fragmenter fragmenter;
  link::Reassembler gateway_link;
  assert(fragmenter.load(frame, n, 0x42U));
  uint8_t packet[link::kFrameSize]{};
  uint8_t size = 0U;
  size_t fragments = 0;
  while (fragmenter.pending()) {
    assert(fragmenter.peek(packet, size));
    assert(size <= 20U);
    const auto result = gateway_link.push(
        packet, size, 1000U + static_cast<uint32_t>(fragments));
    fragmenter.advance();
    ++fragments;
    assert(result == (fragmenter.pending()
                          ? link::Reassembler::Result::kNeedMore
                          : link::Reassembler::Result::kComplete));
  }
  assert(fragments == 6U);
  assert(gateway_link.size() == n);
  assert(memcmp(gateway_link.message(), frame, n) == 0);

  wire::UntrustedOfferView view{};
  assert(wire::inspectOffer(gateway_link.message(), n, view));
  assert(view.receipt_size == 44U && view.source_row == 1U);

  FakeSignedEnrollment credentials;
  relay::GatewayRamRelay queue(kGateway);
  assert(queue.accept(gateway_link.message(), n, 1000U, true, credentials) ==
         relay::Admission::kQueued);
  assert(queue.accept(gateway_link.message(), n, 1001U, true, credentials) ==
         relay::Admission::kDuplicate);
  assert(queue.pending(1001U) == 1U); // No durable STAGED or Tracker ACK.

  relay::TxConditions policy{};
  relay::Candidate selected{};
  policy.gateway_service_authorized = true;
  policy.radio_idle = true;
  policy.custody_ack_pending = false;
  policy.command_pending = false;
  policy.live_pending = false;
  policy.regional_rf_budget_granted = true;
  policy.tracker_rx_window_authorized = true;
  policy.heard_tracker = kTracker;
  policy.heard_age_ms = 1000U;
  assert(queue.select(1500U, policy, credentials, selected));
  assert(selected.source_row == 1U && selected.length == 44U);
  assert(memcmp(selected.wire, view.receipt, 44U) == 0);
  // The TX owner has not launched anything; selection must not consume RAM.
  assert(queue.pending(1501U) == 1U);
  policy.custody_ack_pending = true;
  assert(!queue.select(1501U, policy, credentials, selected));
  assert(selected.length == 0U);
  policy.custody_ack_pending = false;

  uint8_t changed[160]{};
  memcpy(changed, frame, n);
  changed[n - 1U] ^= 0x80U; // Real Edge HMAC corruption.
  assert(queue.accept(changed, n, 1502U, true, credentials) ==
         relay::Admission::kDenied);
  changed[n - 1U] ^= 0x80U;
  changed[1U + 19U + 20U] ^= 1U; // Signed credential epoch mismatch.
  assert(queue.accept(changed, n, 1503U, true, credentials) ==
         relay::Admission::kDenied);
  assert(queue.pending(1503U) == 1U);

  credentials.signed_snapshot = false; // Revocation.
  assert(!queue.select(1504U, policy, credentials, selected));
  assert(queue.pending(1504U) == 0U);
}
} // namespace

int main(int argc, char** argv) {
  assert(argc == 2 && argv[1] != nullptr);
  uint8_t fixture[160]{};
  const size_t n = readFixture(argv[1], fixture);
  testInterop(fixture, n);
  puts("KQ05 real Python Backend->Edge offer -> native 20B BLE -> Gateway HMAC RAM: PASS (HOST ONLY)");
  return 0;
}
