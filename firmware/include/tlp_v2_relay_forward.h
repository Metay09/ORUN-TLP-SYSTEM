#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace tlp {

// SF5B section 11: version 2 type 0x02 one-hop, opaque relay envelope.
// No custody, decrypt, authentication, or transmission authority is granted.
constexpr uint8_t kV2RelayForwardVersion = 0x02U;
constexpr uint8_t kV2RelayForwardType = 0x02U;
constexpr uint8_t kV2RelayHopCount = 1U;
constexpr size_t kV2RelayForwardHeaderSize = 16U;
constexpr size_t kV2RelayForwardMaxInnerSize = kProductSecureMaxFrameSize;
constexpr size_t kV2RelayForwardMaxSize =
    kV2RelayForwardHeaderSize + kV2RelayForwardMaxInnerSize;
static_assert(kV2RelayForwardMaxSize == 116U,
              "SF5B V2 relay maximum payload changed");

enum class V2RelayCodecStatus : uint8_t {
  kOk,
  kLength,
  kVersion,
  kType,
  kHopCount,
  kReserved,
  kInnerType,
  kInnerInvalid,
  kRelayNotAllowed,
};

struct V2RelayForward {
  uint64_t relay_device_id = 0U;
  int16_t ingress_rssi_dbm = 0;
  int8_t ingress_snr_db = 0;
  uint8_t inner_length = 0U;
  uint8_t inner[kV2RelayForwardMaxInnerSize]{};
};

// Checks the frozen inner allow-list (delegated 0x01, product 0x04, ACK
// 0x05) and its RELAY_ALLOWED bit. Header checks are structural only:
// gateway AEAD admission and tracker HMAC custody verification remain separate.
V2RelayCodecStatus validateV2RelayInner(const uint8_t* bytes, size_t length);

// A successful encoder writes exactly (16 + inner_length) bytes to output and
// returns that length. Failure does not modify output or output_length.
bool encodeV2RelayForward(const V2RelayForward& packet, uint8_t* output,
                          size_t capacity, size_t& output_length);

// A successful decoder copies the exact opaque inner bytes. On any rejected
// wrapper or inner shape, output is unchanged.
V2RelayCodecStatus decodeV2RelayForward(
    const uint8_t* bytes, size_t length, V2RelayForward& output);

}  // namespace tlp
}  // namespace orun_tlp
