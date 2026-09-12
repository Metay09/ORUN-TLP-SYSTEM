#include "tlp_test_packet.h"

namespace orun_tlp::tlp {
namespace {

void writeU32BigEndian(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

void writeU64BigEndian(uint8_t* output, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    output[index] = static_cast<uint8_t>(value >> (56 - (index * 8)));
  }
}

uint32_t readU32BigEndian(const uint8_t* input) {
  return (static_cast<uint32_t>(input[0]) << 24) |
         (static_cast<uint32_t>(input[1]) << 16) |
         (static_cast<uint32_t>(input[2]) << 8) |
         static_cast<uint32_t>(input[3]);
}

uint64_t readU64BigEndian(const uint8_t* input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value = (value << 8) | input[index];
  }
  return value;
}

}  // namespace

bool serializeTestPacket(const TestPacket& packet, uint8_t* output,
                         size_t output_size) {
  if (output == nullptr || output_size != kTestPacketSize) {
    return false;
  }

  output[0] = kProtocolVersion;
  output[1] = kPacketTypeTest;
  writeU64BigEndian(&output[2], packet.source_device_id);
  writeU32BigEndian(&output[10], packet.sequence_number);
  writeU32BigEndian(&output[14], packet.uptime_ms);
  return true;
}

bool deserializeTestPacket(const uint8_t* input, size_t input_size,
                           TestPacket* packet) {
  if (input == nullptr || packet == nullptr || input_size != kTestPacketSize ||
      input[0] != kProtocolVersion || input[1] != kPacketTypeTest) {
    return false;
  }

  packet->source_device_id = readU64BigEndian(&input[2]);
  packet->sequence_number = readU32BigEndian(&input[10]);
  packet->uptime_ms = readU32BigEndian(&input[14]);
  return true;
}

}  // namespace orun_tlp::tlp
