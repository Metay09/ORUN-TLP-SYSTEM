#include "tlp_position_packet.h"

#include "tlp_test_packet.h"

namespace orun_tlp::tlp {
namespace {

constexpr int32_t kMinLatitudeE7 = -900000000;
constexpr int32_t kMaxLatitudeE7 = 900000000;
constexpr int32_t kMinLongitudeE7 = -1800000000;
constexpr int32_t kMaxLongitudeE7 = 1800000000;

bool coordinatesAreInRange(int32_t latitude_e7, int32_t longitude_e7) {
  return latitude_e7 >= kMinLatitudeE7 && latitude_e7 <= kMaxLatitudeE7 &&
         longitude_e7 >= kMinLongitudeE7 && longitude_e7 <= kMaxLongitudeE7;
}

void writeU16BigEndian(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

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

uint16_t readU16BigEndian(const uint8_t* input) {
  return (static_cast<uint16_t>(input[0]) << 8) | input[1];
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

bool serializePositionPacket(const PositionPacket& packet, uint8_t* output,
                             size_t output_size) {
  if (output == nullptr || output_size != kPositionPacketSize ||
      !coordinatesAreInRange(packet.latitude_e7, packet.longitude_e7) ||
      (packet.flags & static_cast<uint8_t>(~0x07U)) != 0) {
    return false;
  }

  output[0] = kProtocolVersion;
  output[1] = kPacketTypePosition;
  writeU64BigEndian(&output[2], packet.source_device_id);
  writeU32BigEndian(&output[10], packet.sequence_number);
  writeU32BigEndian(&output[14], packet.gnss_utc_epoch_seconds);
  writeU32BigEndian(&output[18], static_cast<uint32_t>(packet.latitude_e7));
  writeU32BigEndian(&output[22], static_cast<uint32_t>(packet.longitude_e7));
  writeU32BigEndian(&output[26], static_cast<uint32_t>(packet.altitude_mm));
  writeU16BigEndian(&output[30], packet.hdop_x100);
  output[32] = packet.satellites;
  output[33] = packet.flags;
  return true;
}

bool deserializePositionPacket(const uint8_t* input, size_t input_size,
                               PositionPacket* packet) {
  if (input == nullptr || packet == nullptr || input_size != kPositionPacketSize ||
      input[0] != kProtocolVersion || input[1] != kPacketTypePosition) {
    return false;
  }

  packet->source_device_id = readU64BigEndian(&input[2]);
  packet->sequence_number = readU32BigEndian(&input[10]);
  packet->gnss_utc_epoch_seconds = readU32BigEndian(&input[14]);
  packet->latitude_e7 = static_cast<int32_t>(readU32BigEndian(&input[18]));
  packet->longitude_e7 = static_cast<int32_t>(readU32BigEndian(&input[22]));
  packet->altitude_mm = static_cast<int32_t>(readU32BigEndian(&input[26]));
  packet->hdop_x100 = readU16BigEndian(&input[30]);
  packet->satellites = input[32];
  packet->flags = input[33];
  return coordinatesAreInRange(packet->latitude_e7, packet->longitude_e7) &&
         (packet->flags & static_cast<uint8_t>(~0x07U)) == 0;
}

}  // namespace orun_tlp::tlp
