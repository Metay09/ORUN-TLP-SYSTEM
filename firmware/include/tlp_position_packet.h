#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp::tlp {

constexpr uint8_t kPacketTypePosition = 0x02;
constexpr size_t kPositionPacketSize = 34;

constexpr uint8_t kPositionFlagValidFix = 1U << 0;
constexpr uint8_t kPositionFlagValidUtcTime = 1U << 1;
constexpr uint8_t kPositionFlag3dFix = 1U << 2;

struct PositionPacket {
  uint64_t source_device_id;
  uint32_t sequence_number;
  uint32_t gnss_utc_epoch_seconds;
  int32_t latitude_e7;
  int32_t longitude_e7;
  int32_t altitude_mm;
  uint16_t hdop_x100;
  uint8_t satellites;
  uint8_t flags;
};

bool serializePositionPacket(const PositionPacket& packet, uint8_t* output,
                             size_t output_size);
bool deserializePositionPacket(const uint8_t* input, size_t input_size,
                               PositionPacket* packet);

}  // namespace orun_tlp::tlp
