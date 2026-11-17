#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

namespace orun_tlp::kq05_edge_link {

// KQ05 experimental logical edge_link envelope, NOT an active delivery path.
// Existing SF5L6 BLE Reassembler/Fragmenter split logical bytes into <=20B
// ATT frames. These inspectors do NOT authenticate HMAC or authorize storage.
constexpr uint8_t kOfferEnvelopeType = 0x91U;  // Edge -> Gateway
constexpr uint8_t kStageEnvelopeType = 0x08U;  // Gateway -> Edge
constexpr uint8_t kVersion = 1U;
constexpr uint8_t kInnerOfferType = 0x91U;
constexpr uint8_t kInnerStageType = 0x92U;
constexpr size_t kMinReceiptBytes = 44U;
constexpr size_t kMaxReceiptBytes = 72U;
constexpr size_t kOfferPrefixBytes = 19U;
constexpr size_t kOfferTagBytes = 32U;
constexpr size_t kMinOfferMessageBytes =
    1U + kOfferPrefixBytes + kMinReceiptBytes + kOfferTagBytes;  // 96
constexpr size_t kMaxOfferMessageBytes =
    1U + kOfferPrefixBytes + kMaxReceiptBytes + kOfferTagBytes;  // 124
constexpr size_t kStageInnerBytes = 83U;
constexpr size_t kStageMessageBytes = 1U + kStageInnerBytes;     // 84

// Pointers are borrowed from the logical frame; immediately copy if needed.
// Positive inspection is STRUCTURAL ONLY. Never ACK, enqueue or free Flash.
struct UntrustedOfferView {
  const uint8_t* gateway = nullptr;  // 8B
  uint64_t source_row = 0;
  const uint8_t* receipt = nullptr;
  size_t receipt_size = 0;
  const uint8_t* link_mac = nullptr; // 32B, NOT VERIFIED HERE
};

struct UntrustedStageView {
  const uint8_t* gateway = nullptr;  // 8B
  uint64_t source_row = 0;
  uint8_t code = 0;               // 1=STAGED, 2=ALREADY (UNTRUSTED)
  const uint8_t* offer_sha256 = nullptr; // 32B, NOT VERIFIED
  const uint8_t* link_mac = nullptr; // 32B, NOT VERIFIED HERE
};

// Wrap a Python candidate's exact 95..123B authenticated offer as a
// 96..124B SF5L6 logical message. The Gateway still needs the independent
// Edge/Gateway HMAC verifier and a durable NOR owner before staging.
size_t wrapOffer(const uint8_t* inner, size_t length,
                 uint8_t* output, size_t output_capacity);

// Wrap 83B Gateway STAGED reply only after a real authenticated durable
// stage/commit/readback has succeeded. This codec has no such authority.
size_t wrapStageResult(const uint8_t* inner, size_t length,
                       uint8_t* output, size_t output_capacity);

bool inspectOffer(const uint8_t* message, size_t length,
                  UntrustedOfferView& output);
bool inspectStageResult(const uint8_t* message, size_t length,
                        UntrustedStageView& output);

}  // namespace orun_tlp::kq05_edge_link

#endif  // ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
