#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "edge_link.h"
#include "kq05_edge_link_wire.h"

#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
#error "Only the isolated KQ05 lab target may use this test"
#endif

using orun_tlp::edge_link::Fragmenter;
using orun_tlp::edge_link::Reassembler;
namespace wire = orun_tlp::kq05_edge_link;

namespace {
constexpr uint8_t kGateway[8] = {'G','A','T','E','W','A','Y','1'};
constexpr uint8_t kTracker[8] = {'T','R','A','C','K','E','R','1'};
constexpr uint8_t kIncarnation[8] = {'I','N','C','A','R','N','0','1'};

void fillOffer(uint8_t* data, uint8_t count) {
  const size_t receipt_len = 40U + 4U * count;
  memset(data, 0, 19U + receipt_len + 32U);
  data[0] = 1U;
  data[1] = 0x91U;
  memcpy(data + 2U, kGateway, sizeof(kGateway));
  data[17] = 1U;  // Edge source row ID=1, big endian 8 bytes
  data[18] = static_cast<uint8_t>(receipt_len);
  uint8_t* receipt = data + 19U;
  receipt[0] = 2U; receipt[1] = 9U; receipt[2] = 0U;
  receipt[3] = count;
  memcpy(receipt + 4U, kTracker, sizeof(kTracker));
  memcpy(receipt + 12U, kIncarnation, sizeof(kIncarnation));
  receipt[23] = 7U;  // non-FF credential key epoch=7
  for (uint8_t i = 0; i < count; ++i)
    receipt[24U + 4U*i + 3U] = static_cast<uint8_t>(101U + i);
  for (size_t i = 0; i < 16U; ++i)
    receipt[24U + 4U*count + i] = static_cast<uint8_t>(0x20U + i);
  for (size_t i = 0; i < 32U; ++i)
    data[19U + receipt_len + i] = static_cast<uint8_t>(0x60U + i);
}

void fragmentationRoundtrip(const uint8_t* message, size_t size) {
  Fragmenter fragments;
  Reassembler receiver;
  assert(fragments.load(message, size, 0x42U));
  uint8_t frame[orun_tlp::edge_link::kFrameSize]{};
  uint8_t len = 0U;
  bool completed = false;
  unsigned count = 0U;
  while (fragments.pending()) {
    assert(fragments.peek(frame, len));
    assert(len >= 5U && len <= 20U);
    const auto result = receiver.push(frame, len, 1000U + count);
    fragments.advance();
    ++count;
    if (fragments.pending())
      assert(result == Reassembler::Result::kNeedMore);
    else {
      assert(result == Reassembler::Result::kComplete);
      completed = true;
    }
  }
  assert(completed && receiver.size() == size);
  assert(memcmp(receiver.message(), message, size) == 0);
  assert(count == (size + 15U) / 16U);
}

void testOffer() {
  uint8_t raw[123]{};
  fillOffer(raw, 1U);
  uint8_t wrapped[orun_tlp::edge_link::kMaxMessage]{};
  assert(wire::wrapOffer(raw, 95U, wrapped, sizeof(wrapped)) == 96U);
  assert(wrapped[0] == wire::kOfferEnvelopeType);
  wire::UntrustedOfferView view{};
  assert(wire::inspectOffer(wrapped, 96U, view));
  assert(view.source_row == 1U && view.receipt_size == 44U);
  assert(memcmp(view.gateway, kGateway, 8U) == 0);
  assert(memcmp(view.receipt + 4U, kTracker, 8U) == 0);
  assert(view.link_mac == wrapped + 96U - 32U);
  fragmentationRoundtrip(wrapped, 96U);

  // Structural parsing does not confer cryptographic authority!
  wrapped[95] ^= 1U;
  assert(wire::inspectOffer(wrapped, 96U, view));
  wrapped[95] ^= 1U;

  uint8_t changed[sizeof(wrapped)]{};
  memcpy(changed, wrapped, 96U);
  changed[0] = 0x83U;
  assert(!wire::inspectOffer(changed, 96U, view));
  changed[0] = wire::kOfferEnvelopeType;
  changed[1] = 2U;
  assert(!wire::inspectOffer(changed, 96U, view));
  changed[1] = 1U;
  changed[19] = 45U;   // wrong embedded receipt size
  assert(!wire::inspectOffer(changed, 96U, view));
  changed[19] = 44U;
  changed[23] = 0U;    // no sequence entries
  assert(!wire::inspectOffer(changed, 96U, view));
  changed[23] = 1U;
  changed[47] = 0U;    // zero sequence
  assert(!wire::inspectOffer(changed, 96U, view));
  changed[47] = 101U;
  assert(wire::inspectOffer(changed, 96U, view));
  assert(!wire::inspectOffer(nullptr, 96U, view));
  assert(!wire::inspectOffer(wrapped, 95U, view));
  assert(wire::wrapOffer(raw, 95U, wrapped, 95U) == 0U);

  fillOffer(raw, 8U);
  assert(wire::wrapOffer(raw, 123U, wrapped, sizeof(wrapped)) == 124U);
  assert(wire::inspectOffer(wrapped, 124U, view));
  assert(view.receipt_size == 72U);
  fragmentationRoundtrip(wrapped, 124U);
}

void testStage() {
  uint8_t raw[83]{};
  raw[0] = 1U;
  raw[1] = 0x92U;
  memcpy(raw + 2U, kGateway, 8U);
  raw[17] = 1U;
  raw[18] = 1U;
  for (size_t i = 0; i < 64U; ++i)
    raw[19U + i] = static_cast<uint8_t>(i + 1U);
  uint8_t wrapped[orun_tlp::edge_link::kMaxMessage]{};
  assert(wire::wrapStageResult(raw, sizeof(raw), wrapped, sizeof(wrapped)) ==
         wire::kStageMessageBytes);
  wire::UntrustedStageView view{};
  assert(wire::inspectStageResult(wrapped, 84U, view));
  assert(view.source_row == 1U && view.code == 1U);
  assert(view.offer_sha256 == wrapped + 20U);
  assert(view.link_mac == wrapped + 52U);
  fragmentationRoundtrip(wrapped, 84U);

  wrapped[19] = 3U;
  assert(!wire::inspectStageResult(wrapped, 84U, view));
  wrapped[19] = 2U;
  assert(wire::inspectStageResult(wrapped, 84U, view));
  wrapped[0] = 0x04U;  // not SF5L6 ACCEPT_RESULT
  assert(!wire::inspectStageResult(wrapped, 84U, view));
  assert(wire::wrapStageResult(raw, sizeof(raw), wrapped, 83U) == 0U);
}
}  // namespace

int main() {
  testOffer();
  testStage();
  return 0;
}
