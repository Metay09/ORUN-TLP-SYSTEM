#include "tlp_relay_forward_packet.h"

#include <string.h>

#include "tlp_test_packet.h"

namespace orun_tlp::tlp {
namespace {

void writeU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

void writeU64(uint8_t* output, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    output[index] = static_cast<uint8_t>(value >> (56 - 8 * index));
  }
}

uint16_t readU16(const uint8_t* input) {
  return static_cast<uint16_t>((uint16_t(input[0]) << 8) | input[1]);
}

uint64_t readU64(const uint8_t* input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) value = (value << 8) | input[index];
  return value;
}

}  // namespace

bool serializeRelayForwardPacket(const RelayForwardPacket& packet,
                                 uint8_t* output, size_t output_size) {
  PositionPacket position{};
  if (output == nullptr || output_size != kRelayForwardPacketSize ||
      packet.hop_count != kRelayHopCount ||
      packet.original_length != kPositionPacketSize ||
      !deserializePositionPacket(packet.original_packet,
                                 packet.original_length, &position)) {
    return false;
  }
  output[0] = kProtocolVersion;
  output[1] = kPacketTypeRelayForward;
  writeU64(&output[2], packet.relay_device_id);
  output[10] = packet.hop_count;
  output[11] = packet.original_length;
  writeU16(&output[12], static_cast<uint16_t>(packet.ingress_rssi_dbm));
  output[14] = static_cast<uint8_t>(packet.ingress_snr_db);
  memcpy(&output[kRelayForwardHeaderSize], packet.original_packet,
         kPositionPacketSize);
  return true;
}

RelayDecodeStatus deserializeRelayForwardPacket(const uint8_t* input,
                                                 size_t input_size,
                                                 RelayForwardPacket* packet,
                                                 PositionPacket* position) {
  if (input == nullptr || packet == nullptr || position == nullptr ||
      input_size < kRelayForwardHeaderSize) return RelayDecodeStatus::kLength;
  if (input[0] != kProtocolVersion) return RelayDecodeStatus::kVersion;
  if (input[1] != kPacketTypeRelayForward) return RelayDecodeStatus::kType;
  if (input[10] != kRelayHopCount) return RelayDecodeStatus::kHopCount;
  if (input[11] != kPositionPacketSize)
    return RelayDecodeStatus::kOriginalLength;
  if (input_size != kRelayForwardHeaderSize + input[11])
    return RelayDecodeStatus::kLength;
  const uint8_t* original = &input[kRelayForwardHeaderSize];
  if (original[0] != kProtocolVersion) return RelayDecodeStatus::kInnerVersion;
  if (original[1] == kPacketTypeRelayForward)
    return RelayDecodeStatus::kNestedRelay;
  if (original[1] != kPacketTypePosition)
    return RelayDecodeStatus::kInnerType;
  if (!deserializePositionPacket(original, input[11], position))
    return RelayDecodeStatus::kMalformedPosition;

  packet->relay_device_id = readU64(&input[2]);
  packet->hop_count = input[10];
  packet->original_length = input[11];
  packet->ingress_rssi_dbm = static_cast<int16_t>(readU16(&input[12]));
  packet->ingress_snr_db = static_cast<int8_t>(input[14]);
  memcpy(packet->original_packet, original, kPositionPacketSize);
  return RelayDecodeStatus::kOk;
}

}  // namespace orun_tlp::tlp
