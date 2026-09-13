#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_position_packet.h"

namespace orun_tlp::tlp {

constexpr uint8_t kPacketTypeRelayForward = 0x03;
constexpr uint8_t kRelayHopCount = 1;
constexpr size_t kRelayForwardHeaderSize = 15;
constexpr size_t kRelayForwardPacketSize =
    kRelayForwardHeaderSize + kPositionPacketSize;

struct RelayForwardPacket {
  uint64_t relay_device_id = 0;
  uint8_t hop_count = kRelayHopCount;
  uint8_t original_length = kPositionPacketSize;
  int16_t ingress_rssi_dbm = 0;
  int8_t ingress_snr_db = 0;
  uint8_t original_packet[kPositionPacketSize]{};
};

enum class RelayDecodeStatus : uint8_t {
  kOk,
  kLength,
  kVersion,
  kType,
  kHopCount,
  kOriginalLength,
  kNestedRelay,
  kInnerVersion,
  kInnerType,
  kMalformedPosition,
};

bool serializeRelayForwardPacket(const RelayForwardPacket& packet,
                                 uint8_t* output, size_t output_size);
RelayDecodeStatus deserializeRelayForwardPacket(const uint8_t* input,
                                                 size_t input_size,
                                                 RelayForwardPacket* packet,
                                                 PositionPacket* position);

}  // namespace orun_tlp::tlp
