#include "kq05_edge_link_wire.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

#include <string.h>

#include "edge_link.h"

namespace orun_tlp::kq05_edge_link {
namespace {
static_assert(kMaxOfferMessageBytes <= edge_link::kMaxMessage,
              "KQ05 offer exceeds established BLE logical cap");
static_assert(kStageMessageBytes <= edge_link::kMaxMessage,
              "KQ05 stage result exceeds established BLE logical cap");

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p) {
  uint64_t v = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) v = (v << 8U) | p[i];
  return v;
}
bool nonzero8(const uint8_t* p) {
  uint8_t sum = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) sum = uint8_t(sum | p[i]);
  return sum != 0U;
}

bool receiptShape(const uint8_t* wire, size_t n) {
  if (wire == nullptr || n < kMinReceiptBytes || n > kMaxReceiptBytes ||
      wire[0] != 2U || wire[1] != 9U || wire[2] != 0U ||
      wire[3] == 0U || wire[3] > 8U ||
      n != 40U + 4U * wire[3] || !nonzero8(wire + 4U) ||
      !nonzero8(wire + 12U) || be32(wire + 20U) == UINT32_MAX)
    return false;
  uint32_t previous = 0U;
  for (uint8_t i = 0U; i < wire[3]; ++i) {
    const uint32_t next = be32(wire + 24U + 4U * i);
    if (next == 0U || next <= previous) return false;
    previous = next;
  }
  return true;  // Does NOT validate the 16B Backend HMAC.
}

bool inspectInnerOffer(const uint8_t* p, size_t n,
                       UntrustedOfferView& output) {
  if (p == nullptr || n < kOfferPrefixBytes + kMinReceiptBytes +
                                     kOfferTagBytes ||
      n > kOfferPrefixBytes + kMaxReceiptBytes + kOfferTagBytes ||
      p[0] != kVersion || p[1] != kInnerOfferType ||
      !nonzero8(p + 2U))
    return false;
  const uint64_t row = be64(p + 10U);
  const size_t receipt_size = p[18U];
  if (row == 0U || row > INT64_MAX ||
      receipt_size < kMinReceiptBytes ||
      receipt_size > kMaxReceiptBytes ||
      n != kOfferPrefixBytes + receipt_size + kOfferTagBytes ||
      !receiptShape(p + kOfferPrefixBytes, receipt_size))
    return false;

  output.gateway = p + 2U;
  output.source_row = row;
  output.receipt = p + kOfferPrefixBytes;
  output.receipt_size = receipt_size;
  output.link_mac = p + kOfferPrefixBytes + receipt_size;
  return true;
}

bool inspectInnerStage(const uint8_t* p, size_t n,
                       UntrustedStageView& output) {
  if (p == nullptr || n != kStageInnerBytes ||
      p[0] != kVersion || p[1] != kInnerStageType ||
      !nonzero8(p + 2U))
    return false;
  const uint64_t row = be64(p + 10U);
  if (row == 0U || row > INT64_MAX || (p[18] != 1U && p[18] != 2U))
    return false;

  output.gateway = p + 2U;
  output.source_row = row;
  output.code = p[18U];
  output.offer_sha256 = p + 19U;
  output.link_mac = p + 51U;
  return true;  // Does NOT authenticate STAGED or confirm Flash durability.
}
}  // namespace

size_t wrapOffer(const uint8_t* inner, size_t length,
                 uint8_t* out, size_t cap) {
  UntrustedOfferView view{};
  if (out == nullptr || cap < length + 1U ||
      !inspectInnerOffer(inner, length, view)) return 0U;
  memmove(out + 1U, inner, length);
  out[0] = kOfferEnvelopeType;
  return length + 1U;
}

size_t wrapStageResult(const uint8_t* inner, size_t length,
                       uint8_t* out, size_t cap) {
  UntrustedStageView view{};
  if (out == nullptr || cap < length + 1U ||
      !inspectInnerStage(inner, length, view)) return 0U;
  memmove(out + 1U, inner, length);
  out[0] = kStageEnvelopeType;
  return length + 1U;
}

bool inspectOffer(const uint8_t* message, size_t length,
                  UntrustedOfferView& output) {
  output = {};
  if (message == nullptr || length < kMinOfferMessageBytes ||
      length > kMaxOfferMessageBytes ||
      message[0] != kOfferEnvelopeType)
    return false;
  return inspectInnerOffer(message + 1U, length - 1U, output);
}

bool inspectStageResult(const uint8_t* message, size_t length,
                        UntrustedStageView& output) {
  output = {};
  if (message == nullptr || length != kStageMessageBytes ||
      message[0] != kStageEnvelopeType)
    return false;
  return inspectInnerStage(message + 1U, length - 1U, output);
}

}  // namespace orun_tlp::kq05_edge_link
#endif  // ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
